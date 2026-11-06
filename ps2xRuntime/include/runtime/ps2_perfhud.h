#pragma once
// [perfhud] numbers for the overlay's performance readout (see ps2_perfhud.cpp)
struct PerfHudStats
{
    double fps = 0, frameMs = 0;          // game frames per second (the game's own frame counter)
    double pictures = 0;                  // distinct pictures shown per second (native renderer; 0 = not presenting)
    double cpuGame = 0, cpuTotal = 0;     // % of all cores: this process / whole PC
    double ramMB = 0;                     // working set
    double gpuGame = 0, gpuTotal = 0;     // % 3D engine: this process / all processes
    double vramMB = 0;                    // dedicated VRAM used by this process
    int gpuTempC = -1;                    // NVIDIA only (-1 = unknown)
    bool haveGpu = false;
};
PerfHudStats ps2xPerfHudStats();   // latest 1-second sample; starts the sampler on first call
