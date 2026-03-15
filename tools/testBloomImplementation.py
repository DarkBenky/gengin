import numpy as np
import matplotlib.pyplot as plt
import time

def decimate(image, factor):
    start = time.perf_counter()
    h = (image.shape[0] // factor) * factor
    w = (image.shape[1] // factor) * factor
    C = image.shape[2]
    result = image[:h, :w].reshape(h // factor, factor, w // factor, factor, C).mean(axis=(1, 3)).astype(image.dtype)
    print(f"decimate: {time.perf_counter() - start:.3f}s")
    return result

def threshold(image, threshold):
    start = time.perf_counter()
    mask = np.any(image > threshold, axis=-1, keepdims=True)
    result = np.where(mask, np.float32(1.0), np.float32(0.0))
    print(f"threshold: {time.perf_counter() - start:.3f}s")
    return result

def bilinearUpsample(image, factor):
    start = time.perf_counter()
    h, w, C = image.shape
    # horizontal pass: (h, w, C) -> (h, w*factor, C)
    tj = np.arange(w * factor)
    y0 = tj // factor
    y1 = np.minimum(y0 + 1, w - 1)
    dy = ((tj % factor) / factor).reshape(1, -1, 1)
    horiz = image[:, y0] * (1.0 - dy) + image[:, y1] * dy
    # vertical pass: (h, w*factor, C) -> (h*factor, w*factor, C)
    ti = np.arange(h * factor)
    x0 = ti // factor
    x1 = np.minimum(x0 + 1, h - 1)
    dx = ((ti % factor) / factor).reshape(-1, 1, 1)
    result = (horiz[x0] * (1.0 - dx) + horiz[x1] * dx).astype(image.dtype)
    print(f"bilinearUpsample: {time.perf_counter() - start:.3f}s")
    return result

def average_images(images):
    start = time.perf_counter()
    h = max(img.shape[0] for img in images)
    w = max(img.shape[1] for img in images)
    sum_image = np.zeros((h, w, images[0].shape[2]), dtype=np.float64)
    for img in images:
        ph = h - img.shape[0]
        pw = w - img.shape[1]
        if ph or pw:
            img = np.pad(img, ((0, ph), (0, pw), (0, 0)))
        sum_image += img
    
    img = (sum_image / len(images)).astype(images[0].dtype)
    print(f"average_images: {time.perf_counter() - start:.3f}s")
    return img

def average_images_by_weight(images, weights):
    start = time.perf_counter()
    h = max(img.shape[0] for img in images)
    w = max(img.shape[1] for img in images)
    sum_image = np.zeros((h, w, images[0].shape[2]), dtype=np.float64)
    total_weight = sum(weights)
    for img, wt in zip(images, weights):
        ph = h - img.shape[0]
        pw = w - img.shape[1]
        if ph or pw:
            img = np.pad(img, ((0, ph), (0, pw), (0, 0)))
        sum_image += img * wt
    img = (sum_image / total_weight if total_weight > 0 else sum_image).astype(images[0].dtype)
    print(f"average_images_by_weight: {time.perf_counter() - start:.3f}s")
    return img

def add_images(image1, image2):
    start = time.perf_counter()
    h, w = image1.shape[:2]
    ph = max(0, h - image2.shape[0])
    pw = max(0, w - image2.shape[1])
    if ph or pw:
        image2 = np.pad(image2, ((0, ph), (0, pw), (0, 0)))
    img = np.clip(image1 + image2[:h, :w], 0.0, 1.0)
    print(f"add_images: {time.perf_counter() - start:.3f}s")
    return img

def box_blur(img, radius):
    start = time.perf_counter()
    out = img.astype(np.float64)
    for axis in [0, 1]:
        n = out.shape[axis]
        c = np.cumsum(out, axis=axis)
        pad_shape = list(out.shape)
        pad_shape[axis] = 1
        c = np.concatenate([np.zeros(pad_shape, dtype=np.float64), c], axis=axis)
        hi = np.minimum(np.arange(n) + radius + 1, n)
        lo = np.maximum(np.arange(n) - radius, 0)
        counts = (hi - lo).reshape([-1 if i == axis else 1 for i in range(out.ndim)])
        out = (np.take(c, hi, axis=axis) - np.take(c, lo, axis=axis)) / counts
    img = out.astype(img.dtype)
    print(f"box_blur: {time.perf_counter() - start:.3f}s")
    return img

def test_bloom_effect(iterations = 3, threshold_value = 0.5):
    # load image with bloom effect applied
    image = plt.imread('img.png')[..., :3]
    images = []
    threshold_image = threshold(image.copy(), threshold_value)
    for i in range(iterations):
        decimated = decimate(threshold_image.copy(), 2)
        upsampled = bilinearUpsample(decimated, 2)
        images.append(upsampled)
    final_image = average_images(images)
    final_image = add_images(image, final_image[:image.shape[0], :image.shape[1]])
    # save the final image to disk
    plt.imsave('final_bloom.png', final_image)

def test_bloom_effect_v2(iterations = 3, threshold_value = 0.5):
    image = plt.imread('img.png')[..., :3]
    bloom = threshold(image.copy(), threshold_value)
    for i in range(iterations):
        decimated = decimate(bloom, 2)
        bloom = bilinearUpsample(decimated, 2)
    final_image = add_images(image, bloom[:image.shape[0], :image.shape[1]])
    # save the final image to disk
    plt.imsave('final_bloom_v2.png', final_image)

def test_bloom_effect_v3(iterations = 3, threshold_value = 0.5):
    image = plt.imread('img.png')[..., :3]
    bloom = threshold(image.copy(), threshold_value)
    for i in range(iterations):
        decimated = decimate(bloom, 2)
    bloom = bilinearUpsample(decimated, 2)
    final_image = add_images(image, bloom[:image.shape[0], :image.shape[1]])
    # save the final image to disk
    plt.imsave('final_bloom_v3.png', final_image)

def test_bloom_effect_v4(iterations = 3, threshold_value = 0.5):
    image = plt.imread('img.png')[..., :3]
    bloom_src = threshold(image.copy(), threshold_value)
    decimated_images = []
    weighs = []
    for i in range(iterations):
        factor = 2 * (i + 1)
        d = decimate(bloom_src.copy(), factor)
        upsample = bilinearUpsample(d, factor)
        decimated_images.append(upsample)
        weighs.append(1.0 / (i + 1))
    final_bloom = average_images_by_weight(decimated_images, weighs)
    final_image = add_images(image, final_bloom)
    # save the final image to disk
    plt.imsave('final_bloom_v4.png', final_image)

def test_bloom_effect_v5(iterations = 3, threshold_value = 0.5):
    image = plt.imread('img.png')[..., :3]
    bloom_src = threshold(image.copy(), threshold_value)
    decimated_images = []
    for i in range(iterations):
        factor = 2 * (i + 1)
        d = decimate(bloom_src.copy(), factor)
        upsample = bilinearUpsample(d, factor)
        decimated_images.append(upsample)
    final_bloom = average_images(decimated_images)
    final_image = add_images(image, final_bloom)
    # save the final image to disk
    plt.imsave('final_bloom_v5.png', final_image)

def test_bloom_effect_v6(radius=32, threshold_value=0.5):
    # single box blur — cheapest O(n) via cumsum
    image = plt.imread('img.png')[..., :3]
    bloom = threshold(image.copy(), threshold_value)
    bloom = box_blur(bloom, radius)
    final_image = add_images(image, bloom)
    plt.imsave('final_bloom_v6.png', final_image)

def test_bloom_effect_v7(passes=3, radius=16, threshold_value=0.5):
    # approximate Gaussian via repeated box blur (3 passes ~= Gaussian)
    image = plt.imread('img.png')[..., :3]
    bloom = threshold(image.copy(), threshold_value).astype(np.float32)
    for _ in range(passes):
        bloom = box_blur(bloom, radius)
    final_image = add_images(image, bloom)
    plt.imsave('final_bloom_v7.png', final_image)

def test_bloom_effect_v8(levels=5, threshold_value=0.5):
    # pyramid bloom: build half-res mip chain, accumulate back up with additive upsample
    image = plt.imread('img.png')[..., :3]
    mips = [threshold(image.copy(), threshold_value).astype(np.float32)]
    for _ in range(levels):
        mips.append(decimate(mips[-1], 2).astype(np.float32))
    bloom = mips[-1]
    for mip in reversed(mips[:-1]):
        up = bilinearUpsample(bloom, 2)
        h, w = mip.shape[:2]
        ph = max(0, h - up.shape[0])
        pw = max(0, w - up.shape[1])
        if ph or pw:
            up = np.pad(up, ((0, ph), (0, pw), (0, 0)))
        bloom = mip + up[:h, :w]
    final_image = add_images(image, bloom[:image.shape[0], :image.shape[1]])
    plt.imsave('final_bloom_v8.png', final_image)

def test_bloom_effect_v9(iterations = 3, threshold_value = 0.5):
    image = plt.imread('img.png')[..., :3]
    bloom = threshold(image.copy(), threshold_value)
    bloom = decimate(bloom, 8)
    for _ in range(iterations):
        bloom = box_blur(bloom, 3)
    bloom = bilinearUpsample(bloom, 8)
    final_image = add_images(image, bloom[:image.shape[0], :image.shape[1]])
    plt.imsave('final_bloom_v9.png', final_image)
    
if __name__ == "__main__":
    iterations = 9
    threshold_value = 0.8
    tests = [
        ("v1", lambda: test_bloom_effect(iterations, threshold_value)),
        ("v2", lambda: test_bloom_effect_v2(iterations, threshold_value)),
        ("v3", lambda: test_bloom_effect_v3(iterations, threshold_value)),
        ("v4", lambda: test_bloom_effect_v4(iterations, threshold_value)),
        ("v5", lambda: test_bloom_effect_v5(iterations, threshold_value)),
        ("v6", lambda: test_bloom_effect_v6(32, threshold_value)),
        ("v7", lambda: test_bloom_effect_v7(3, 16, threshold_value)),
        ("v8", lambda: test_bloom_effect_v8(8, threshold_value)),
        ("v9", lambda: test_bloom_effect_v9(iterations, threshold_value)),
    ]
    for name, fn in tests:
        t0 = time.perf_counter()
        fn()
        print(f"bloom {name}: {time.perf_counter() - t0:.3f}s")