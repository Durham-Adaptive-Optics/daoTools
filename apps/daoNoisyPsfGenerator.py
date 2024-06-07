import numpy as np
from scipy.ndimage import shift
import dao

# Define the size of the image
image_size = 512
# create SHM
shm = dao.shm('/tmp/psf.im.shm', np.zeros((image_size, image_size)).astype(np.uint16))

# Create a 2D grid of coordinates
x = np.linspace(-1, 1, image_size)
y = np.linspace(-1, 1, image_size)
x, y = np.meshgrid(x, y)

# Define the point spread function (PSF) as a 2D Gaussian
sigma = 0.1  # Standard deviation of the Gaussian
psf = np.exp(-(x**2 + y**2) / (2 * sigma**2))

# Normalize the PSF to the max of uint16
psf *= (2**16 - 1) / np.max(psf)

while 1:
    # Randomly shift the PSF by a few pixels (~5 pixels)
    shift_x = np.random.randint(-5, 6)
    shift_y = np.random.randint(-5, 6)
    psf_shifted = shift(psf, shift=[shift_y, shift_x], mode='constant', cval=0)
    
    # Add noise to the image
    noise_level = 10
    noise = noise_level * np.random.normal(size=(image_size, image_size))
    
    # Create the noisy PSF image
    psf_noisy = psf_shifted + noise
    
    # Clip values to the valid range of uint16
    psf_noisy = np.clip(psf_noisy, 0, 2**16 - 1)
    
    # Convert to uint16
    shm.set_data(psf_noisy.astype(np.uint16))

