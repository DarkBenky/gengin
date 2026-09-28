import glob
import random

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import Dataset, DataLoader
from torchvision.io import decode_image
from PIL import Image

import matplotlib.pyplot as plt
import wandb

BATCH_SIZE = 16
C = 256
BLOCKS = 16
SCALE = 2
HR_SIZE = 256
SHOW_IMAGES = False
LEARNING_RATE = 1e-4
EPOCHS = 100
DEVICE = 0

def isValidImage(path, minSize):
    try:
        with Image.open(path) as img:
            if min(img.size) < minSize:
                return False
        with open(path, "rb") as f:
            f.seek(0, 2)
            f.seek(max(f.tell() - 32, 0))
            tail = f.read()
    except OSError:
        return False
    return b"IEND" in tail or b"\xff\xd9" in tail

class ImageDataset(Dataset):
    def __init__(self, dirpath, highResImage=HR_SIZE, scale=SCALE):
        self.highResImage = highResImage
        self.lowResImage = highResImage // scale

        files = glob.glob(f"{dirpath}/*.png") + glob.glob(f"{dirpath}/*.jpg")
        self.files = [f for f in files if isValidImage(f, highResImage)]
        dropped = len(files) - len(self.files)
        if dropped:
            print(f"dropped {dropped} unusable images")

    def __len__(self):
        return len(self.files)

    def __getitem__(self, idx):
        for _ in range(16):
            try:
                img = decode_image(self.files[idx], mode="RGB")
                break
            except (RuntimeError, OSError):
                print(f"skipping unreadable image: {self.files[idx]}")
                idx = random.randrange(len(self.files))
        else:
            raise RuntimeError("too many unreadable images in dataset")

        _, H, W = img.shape

        x = random.randint(0, W - self.highResImage)
        y = random.randint(0, H - self.highResImage)
        hr = img[:, y:y + self.highResImage, x:x + self.highResImage].float() / 255

        hr = torch.rot90(hr, random.randint(0, 3), dims=(-2, -1))
        if random.random() < 0.5:
            hr = hr.flip(-1)

        lr = F.interpolate(hr[None], size=(self.lowResImage,) * 2,
                           mode="bicubic", antialias=True)[0].clamp(0, 1)
        return lr, hr

def buildModel():
    model = nn.Sequential()

    model.append(nn.Conv2d(3, C, kernel_size=3, padding=1))
    model.append(nn.ReLU())

    for _ in range(BLOCKS):
        model.append(nn.Conv2d(C, C, kernel_size=3, padding=1))
        model.append(nn.ReLU())

    model.append(nn.Conv2d(C, 3 * SCALE**2, 3, padding=1))
    model.append(nn.PixelShuffle(SCALE))

    gpuCount = torch.cuda.device_count()
    print(f"CUDA devices: {gpuCount}")
    for i in range(gpuCount):
        props = torch.cuda.get_device_properties(i)
        print(f"  cuda:{i}: {props.name} ({props.total_memory / 1024**3:.1f} GB)")

    modelSize = sum(p.numel() for p in model.parameters() if p.requires_grad) / 1e6
    print(f"Model size: {modelSize:.2f} M")

    modelStructure = str(model)

    model = model.cuda(DEVICE)
    model = nn.DataParallel(model, device_ids=[DEVICE])

    optimizer = torch.optim.Adam(model.parameters(), lr=LEARNING_RATE)
    return model, modelSize, optimizer, modelStructure

def sr(model, lr):
    return model(lr) + F.interpolate(lr, scale_factor=SCALE, mode="bilinear")

def buildComparison(lr, pred, hr, rows=4):
    previews = []
    for i in range(min(rows, lr.shape[0])):
        lrUp = F.interpolate(lr[i][None], size=hr.shape[-2:], mode="nearest")[0]
        previews.append(torch.cat([lrUp, pred[i].clamp(0, 1), hr[i]], dim=2))
    return torch.cat(previews, dim=1)

if __name__ == "__main__":
    dataset = ImageDataset("/media/user/2TB/wt_screenshots")
    loader = DataLoader(dataset, batch_size=BATCH_SIZE, shuffle=True, num_workers=8,
                        pin_memory=True, persistent_workers=True, drop_last=True)
    print(len(dataset))

    if SHOW_IMAGES:
        lr, hr = dataset[0]
        print(lr.shape, hr.shape)
        fig, axes = plt.subplots(1, 2)
        axes[0].imshow(lr.permute(1, 2, 0)); axes[0].set_title(f"LR {lr.shape[-1]}")
        axes[1].imshow(hr.permute(1, 2, 0)); axes[1].set_title(f"HR {hr.shape[-1]}")
        plt.show()

    model, modelSize, optimizer, modelStructure = buildModel()
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=EPOCHS * len(loader))
    wandb.init(project="sr-upscaler", config=dict(C=C, blocks=BLOCKS, scale=SCALE, device=DEVICE,
               lr=LEARNING_RATE, batch=BATCH_SIZE, params_M=modelSize,
               structure=modelStructure))

    print(f"From image {HR_SIZE // SCALE} x {HR_SIZE // SCALE} px to {HR_SIZE} x {HR_SIZE} px")

    step = 0
    for epoch in range(EPOCHS):
        model.train()
        for lr, hr in loader:
            lr, hr = lr.cuda(DEVICE, non_blocking=True), hr.cuda(DEVICE, non_blocking=True)

            out = sr(model, lr)
            loss = F.l1_loss(out, hr)

            optimizer.zero_grad(set_to_none=True)
            loss.backward()
            optimizer.step()
            scheduler.step()

            if step % 50 == 0:
                psnr = -10 * torch.log10(F.mse_loss(out.detach().clamp(0, 1), hr))
                wandb.log({"loss": loss.item(), "psnr": psnr.item(),
                           "lr": scheduler.get_last_lr()[0]}, step=step)

            if step % 250 == 0:
                grid = buildComparison(lr, out.detach(), hr)
                wandb.log({"comparison": wandb.Image(grid.cpu())}, step=step)
            step += 1

        torch.save(model.module.state_dict(), "sr.pt")
        print(f"epoch {epoch} done, loss {loss.item():.4f}")