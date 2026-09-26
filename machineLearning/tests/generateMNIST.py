from torchvision import datasets, transforms
from torch.utils.data import DataLoader

def load_mnist(batch_size=5):
    transform = transforms.ToTensor()
    dataset = datasets.MNIST(
        root="./data",
        train=True,
        download=True,
        transform=transform
    )
    return DataLoader(dataset, batch_size=batch_size, shuffle=True)

import struct
import numpy as np

def generateSamples(path: str):
    dataset = load_mnist(batch_size=1)

    with open(path, "wb") as f:
        f.write(struct.pack("<i", len(dataset.dataset)))
        for image, label in dataset:
            f.write(image.numpy().astype(np.float32).tobytes())  # 28*28 floats
            f.write(struct.pack("<i", int(label.item())))

if __name__ == "__main__":
    generateSamples("data/mnist.bin")