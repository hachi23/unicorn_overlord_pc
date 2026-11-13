# Build environment: `nix-shell shell.nix --run 'bash run.sh'`
{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  packages = with pkgs; [
    gcc gnumake cmake ninja pkg-config python3 binutils
    vulkan-headers vulkan-loader sdl3
    # GPU library (gpu/): window, Vulkan platform, presenter shaders; spirv-tools for the tests
    ffmpeg-headless glslang spirv-tools libx11 libxcb xorgproto wayland
  ];
}
