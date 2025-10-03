/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2025-09-26 09:35:40
 * @ Description: Dao application for downsampling a source image.
 */

#include <daoProfile.hpp>
#include <CLI/CLI.hpp>
#include <assert.h>
#include <signal.h>
#include <string>
#include <dao.h>

DAO_PROFILE(
    downsampleProfile, 
    std::chrono::microseconds, 
    "Processing", "Downsample"
);

struct CliArguments
{
    std::string sourceShmPath;
    std::string outputShmPath;
    bool summationMode = false;
};

void handleInterruptSignal(int signal)
{
    DAO_PROFILE_EXPORT(downsampleProfile);
    printf("exiting..\n");
    exit(0);
}

void
downsample(
    uint16_t *srcImage, size_t srcWidth, 
    uint16_t *outImage, size_t outWidth, size_t outHeight,
    size_t gridWidth, size_t gridHeight,
    bool sumMode
) 
{
    const size_t gridNPixels = gridWidth * gridHeight;
    
    // Loop over each pixel in the output image
    for(size_t dy = 0; dy < outHeight; ++dy) {
        for(size_t dx = 0; dx < outWidth; ++dx) {
            // Find top-left origin pixel for this pixel's source grid.
            size_t go_x = gridWidth * dx;
            size_t go_y = gridHeight * dy;

            // Sum pixels in the grid.
            size_t pixelSum = 0;
            for(size_t sy = go_y; sy < go_y + gridHeight; ++sy) {
                for(size_t sx = go_x; sx < go_x + gridWidth; ++sx) {
                    uint16_t *srcPixel = srcImage + sy * srcWidth + sx;
                    pixelSum += *srcPixel;
                }
            }

            // Store pixel value to the output image, rounding
            // to the nearest integer if doing an average.
            *(outImage + dy * outWidth + dx) = sumMode ? pixelSum : (pixelSum + (gridNPixels / 2)) / gridNPixels;
        }
    }
}

int main(int argc, char *argv[]) 
{
    // Register our signal handler with the OS
    signal(SIGINT, handleInterruptSignal);

    // Parse CLI
    CliArguments args;
    CLI::App cli ("DAO Image Downsampler");
    cli.add_option("sourceImageSHM", args.sourceShmPath, "Path to the DAO shared memory containing the source image.")->required();
    cli.add_option("outputImageSHM", args.outputShmPath, "Path to the DAO shared memory where the downsampled image should be written.")->required();
    cli.add_flag("-s,--sum", args.summationMode, "Sums pixel values when downsampling instead of averaging."); 
    CLI11_PARSE(cli, argc, argv);

    // Open the source image shared memory
    IMAGE sourceShm;
    if(daoShmShm2Img(args.sourceShmPath.c_str(), &sourceShm) != DAO_SUCCESS) {
        printf("Failed to open source image shared memory (%s)!\n", args.sourceShmPath.c_str());
        return EXIT_FAILURE;
    }

    // Open the output image shared memory
    IMAGE outShm;
    if(daoShmShm2Img(args.outputShmPath.c_str(), &outShm) != DAO_SUCCESS) {
        printf("Failed to open output image shared memory (%s)!\n", args.outputShmPath.c_str());
        return EXIT_FAILURE;
    }

    // Ensure output resolution is smaller than input resolution.
    if(sourceShm.md->size[0] < outShm.md->size[0] && sourceShm.md->size[1] < outShm.md->size[1]) {
        printf("Output image (%dx%d) has higher resolution than input image (%dx%d)!\n", 
            outShm.md->size[0], outShm.md->size[1],
            sourceShm.md->size[0], sourceShm.md->size[1]
        );
        return EXIT_FAILURE;
    }

    // Ensure image pixel types are uint16.
    if(sourceShm.md->atype != _DATATYPE_UINT16 && outShm.md->atype != _DATATYPE_UINT16) {
        printf("Images must have unsigned 16-bit integer pixels!\n");
        return EXIT_FAILURE;
    }

    // Ensure downsampling grid will have integral pixel size.
    if(sourceShm.md->size[0] % outShm.md->size[0] && sourceShm.md->size[1] % outShm.md->size[1]) {
        printf("Downsampling from %dx%d to %dx%d gives non-integral grid!\n", 
            sourceShm.md->size[0], sourceShm.md->size[1],
            outShm.md->size[0], outShm.md->size[1]
        );
        return EXIT_FAILURE;
    }

    // Infer downsampling grid.
    const size_t gridHeight = sourceShm.md->size[0] / outShm.md->size[0];
    const size_t gridWidth = sourceShm.md->size[1] / outShm.md->size[1];

    // Allocate memory for downsampled image
    uint16_t *outImage = (uint16_t*)malloc(outShm.md->size[0] * outShm.md->size[1] * sizeof(uint16_t));
    if(!outImage) {
        printf("Failed to allocate memory for downsampled image!\n");
        return EXIT_FAILURE;
    }

    // Info message
    printf("Downsampling.. (src=%dx%d, dst=%dx%d,mode=%s,grid=%ldx%ld)\n", 
        sourceShm.md->size[0], sourceShm.md->size[1],
        outShm.md->size[0], outShm.md->size[1],
        args.summationMode ? "Summation" : "Averaging",
        gridWidth, gridHeight
    );

    // Fetch source image, downsample, and output result.
    volatile IMAGE_METADATA *sourceMetadata = (volatile IMAGE_METADATA*)sourceShm.md;
    uint64_t cnt0 = sourceMetadata->cnt0;
    while(true) {
        uint64_t cnt0_ = sourceMetadata->cnt0;
        if(cnt0_ > cnt0) {
            DAO_PROFILE_NEW_FRAME(downsampleProfile);
            DAO_PROFILE_START(downsampleProfile, "Processing");
            
            cnt0 = cnt0_;

            DAO_PROFILE_START(downsampleProfile, "Downsample");
            downsample(
                sourceShm.array.UI16,
                sourceShm.md->size[1],
                outImage,
                outShm.md->size[1],
                outShm.md->size[0],
                gridWidth,
                gridHeight,
                args.summationMode
            );
            DAO_PROFILE_STOP(downsampleProfile, "Downsample");

            daoShmImage2Shm(outImage, outShm.md->size[0] * outShm.md->size[1], &outShm);
            
            DAO_PROFILE_STOP(downsampleProfile, "Processing");
        }
    }
}
