{
  description = "NitroView (jpeg-nitro) - ultrafast image and vector viewer";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      linuxSystems = [ "x86_64-linux" "aarch64-linux" ];
      allSystems = linuxSystems ++ [ "aarch64-darwin" ];

      forAllSystems = f: nixpkgs.lib.genAttrs allSystems (system: f system);
      forLinuxSystems = f: nixpkgs.lib.genAttrs linuxSystems (system: f system);

      mkEnv = system:
        let
          pkgs = import nixpkgs { inherit system; };
          isLinux = pkgs.stdenv.hostPlatform.isLinux;

          runtimeLibs = pkgs.lib.optionals isLinux (with pkgs; [
            fontconfig
            libGL
            wayland
            libxkbcommon
            libdecor
            dbus
            libx11
            libxext
            libxrandr
            libxcursor
            libxi
            libxfixes
            libxscrnsaver
          ]);

          buildTools = with pkgs; [
            clang
            llvmPackages.bintools
            pkg-config
            xxd
          ] ++ pkgs.lib.optionals isLinux (with pkgs; [
            patchelf
            wayland-protocols
          ]);

          # Deterministic, hash-verified upstream sources for scripts/get-deps.sh
          fetchedDeps = {
            stb_image = pkgs.fetchurl {
              url = "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h";
              sha256 = "sha256-38Kq3aYpX2f2d1h3c/JeW/jIrUJ+0Nt8UYUlyR5y4CA=";
            };
            wuffs = pkgs.fetchurl {
              url = "https://raw.githubusercontent.com/google/wuffs/main/release/c/wuffs-unsupported-snapshot.c";
              sha256 = "sha256-lZ80sQXDARrLg0x6oMMqrUd8RvOBpCmjKAmN1pZflSk=";
            };
            ljt = pkgs.fetchurl {
              url = "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.2/libjpeg-turbo-3.1.2.tar.gz";
              sha256 = "sha256-jwASI0tGTOUIkMSQ8YGU+ROnsfTmoD1mRBefoPhn0M8=";
            };
            sdl3 = pkgs.fetchurl {
              url = "https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz";
              sha256 = "sha256-nHXPFjMDIsIX3t0uBgnxEk8bVLhjPnY0Z7RoTQ9DNKM=";
            };
            dav1d = pkgs.fetchurl {
              url = "https://downloads.videolan.org/pub/videolan/dav1d/1.5.4/dav1d-1.5.4.tar.xz";
              sha256 = "686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5";
            };
            lunasvg = pkgs.fetchurl {
              url = "https://github.com/gereoffy/lunasvg/archive/4c7ed7ad6520bf17c574a3ba6c1d1ce7888a7fc0.tar.gz";
              sha256 = "d085d8dfa1dc8f46b3549e3e7edf793932b32c6f4e5e73c232acb6a2852bf670";
            };
            libcdr = pkgs.fetchurl {
              url = "https://github.com/gereoffy/libcdr/archive/8d58915cc36d5d660859986c7f186228eff7e89c.tar.gz";
              sha256 = "f7bf69a4f021c0ebea3741a34c67e78cd0a1bf21199e2cd271688444f647d506";
            };
            pdfparse3 = pkgs.fetchurl {
              url = "https://github.com/gereoffy/pdfparse3/archive/1a7447e65ca930f23f15e385c7ed4f98713b55bd.tar.gz";
              sha256 = "e1acf0d9aeb7878700abf7f5b06e8a2e56bdedc6f7e700225c3026b671dd4235";
            };
            freetype = pkgs.fetchurl {
              url = "https://github.com/freetype/freetype/archive/refs/tags/VER-2-15-0.tar.gz";
              sha256 = "87c7f1f8699b91e89f773f7dcfa505b044e1dae67f62a55d4e29c3afdcac4fb8";
            };
            brotli = pkgs.fetchurl {
              url = "https://github.com/google/brotli/archive/refs/tags/v1.2.1.tar.gz";
              sha256 = "98d35d11a78dc453aebb7d10c3748f3f73ff78c8267c0dc697b8631f67bf7e8f";
            };
            zstd = pkgs.fetchurl {
              url = "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz";
              sha256 = "eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3";
            };
            drawio_stencils = pkgs.fetchurl {
              url = "https://raw.githubusercontent.com/jgraph/drawio/f411a11f4f6b8994d869bdbc45f4d4d9e2284f34/src/main/webapp/js/stencils.min.js";
              sha256 = "989d72d18bbf02e61c74937d7f51f048d198bb559c3ede4ae66c08e52544b983";
            };
            librevenge = pkgs.fetchurl {
              url = "https://dev-www.libreoffice.org/src/librevenge-0.0.6.tar.bz2";
              sha256 = "52a65e904d255dbdd97a8b7bb28d6574e14f999eb01416aff004502406d0904d";
            };
            lcms2 = pkgs.fetchurl {
              url = "https://dev-www.libreoffice.org/src/lcms2-2.19.1.tar.gz";
              sha256 = "bfc54f7bab59fbc921012014a8032e4cba4abd46db47d46b76416a8c0b2815c8";
            };
            libvisio = pkgs.fetchurl {
              url = "https://github.com/gereoffy/libvisio/archive/7fd688eb9d984a699a08f89a012d919cb626530e.tar.gz";
              sha256 = "4a3a9ba5b7b31a0fb3588bd5987bf7369df2e0d7c1855cd0523d9f2018bf76ba";
            };
            libxml2 = pkgs.fetchurl {
              url = "https://download.gnome.org/sources/libxml2/2.13/libxml2-2.13.8.tar.xz";
              sha256 = "277294cb33119ab71b2bc81f2f445e9bc9435b893ad15bb2cd2b0e859a0ee84a";
            };
            boost = pkgs.fetchurl {
              url = "https://archives.boost.io/release/1.89.0/source/boost_1_89_0.tar.bz2";
              sha256 = "85a33fa22621b4f314f8e85e1a5e2a9363d22e4f4992925d4bb3bc631b5a0c7a";
            };
            mimalloc = pkgs.fetchurl {
              url = "https://github.com/microsoft/mimalloc/archive/refs/tags/v2.1.7.tar.gz";
              sha256 = "0eed39319f139afde8515010ff59baf24de9e47ea316a315398e8027d198202d";
            };
            nasm = pkgs.fetchurl {
              url = "https://www.nasm.us/pub/nasm/releasebuilds/3.02/nasm-3.02.tar.xz";
              sha256 = "039gf7xjrkrw4z13b38lypcm801b1m85var4fj8zxb5lafx6wcw7";
            };
            zlibng = pkgs.fetchurl {
              url = "https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz";
              sha256 = "1l87r6h8qccba1yjx6n13q36yh0wrq3rzz9nnraq5ssjr2lmmipr";
            };
          };
          
          thirdParty = pkgs.clangStdenv.mkDerivation {
            pname = "jpeg-nitro-third-party";
            version = "unstable";
            src = ./.;

            nativeBuildInputs = buildTools ++ (with pkgs; [
              python3 cmake ninja meson nasm
            ]);
            buildInputs = runtimeLibs ++ (with pkgs; [
              libGLU systemdLibs libffi libxtst
            ]);

            dontUseCmakeConfigure = true;
            dontUseMesonConfigure = true;

            buildPhase = ''
              mkdir -p third_party/tools/venv/bin
              cp ${fetchedDeps.stb_image} third_party/stb_image.h
              cp ${fetchedDeps.wuffs} third_party/wuffs.c

              # Copy fetched archives with the exact filenames expected by scripts/get-deps.sh
              cp ${fetchedDeps.lunasvg} third_party/lunasvg.tar.gz
              cp ${fetchedDeps.libcdr} third_party/libcdr.tar.gz
              cp ${fetchedDeps.pdfparse3} third_party/pdfparse3.tar.gz
              cp ${fetchedDeps.freetype} third_party/freetype.tar.gz
              cp ${fetchedDeps.brotli} third_party/brotli.tar.gz
              cp ${fetchedDeps.zstd} third_party/zstd.tar.gz
              cp ${fetchedDeps.librevenge} third_party/librevenge.tar.bz2
              cp ${fetchedDeps.libvisio} third_party/libvisio.tar.gz
              cp ${fetchedDeps.libxml2} third_party/libxml2.tar.xz
              cp ${fetchedDeps.lcms2} third_party/lcms2.tar.gz
              cp ${fetchedDeps.boost} third_party/boost.tar.bz2
              cp ${fetchedDeps.nasm} third_party/nasm.tar.xz
              cp ${fetchedDeps.zlibng} third_party/zlib-ng.tar.gz
              cp ${fetchedDeps.mimalloc} third_party/mimalloc.tar.gz
              cp ${fetchedDeps.ljt} third_party/ljt.tar.gz
              cp ${fetchedDeps.sdl3} third_party/sdl3.tar.gz
              cp ${fetchedDeps.dav1d} third_party/dav1d.tar.xz

              # Expose Nix meson inside third_party/tools/venv/bin so get-deps.sh skips pip
              ln -s ${pkgs.meson}/bin/meson third_party/tools/venv/bin/meson

              # Point gen-drawio-stencils.py to the local fetched stencils.min.js
              sed -i "s|URL = 'https://.*|URL = 'file://${fetchedDeps.drawio_stencils}'|" scripts/gen-drawio-stencils.py

              # Skip curl downloads since all archives are already placed in third_party/
              sed -i 's|curl -fsSL -o [^ ]* [^ ]*|true|g' scripts/get-deps.sh

              bash ./scripts/get-deps.sh
            '';

            installPhase = ''
              mkdir -p $out
              cp -r third_party $out/
            '';
          };
        in
        {
          inherit pkgs isLinux runtimeLibs buildTools fetchedDeps thirdParty;
        };
    in
    {
      packages = forLinuxSystems (system:
        let
          env = mkEnv system;
          inherit (env) pkgs runtimeLibs buildTools fetchedDeps thirdParty;
        in
        {
          nitroview-gnome = pkgs.clangStdenv.mkDerivation {
            pname = "nitroview-gnome";
            version = "unstable";
            src = ./.;

            nativeBuildInputs = buildTools ++ (with pkgs; [ glib.dev wrapGAppsHook4 ]);
            buildInputs = runtimeLibs ++ (with pkgs; [ gtk4 libadwaita glib ]);

            dontUseCmakeConfigure = true;
            dontUseMesonConfigure = true;
            enableParallelBuilding = false;

            buildPhase = ''
              runHook preBuild
              cp -r ${thirdParty}/third_party ./third_party
              chmod -R +w third_party
              make gnome
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin
              cp nitroview-gnome $out/bin/
              runHook postInstall
            '';

            postFixup = ''
              patchelf --set-rpath "$(patchelf --print-rpath $out/bin/nitroview-gnome):${pkgs.lib.makeLibraryPath runtimeLibs}:/run/opengl-driver/lib" $out/bin/nitroview-gnome
            '';
          };

          nitroview-linux = pkgs.clangStdenv.mkDerivation {
            pname = "nitroview-linux";
            version = "unstable";
            src = ./.;

            nativeBuildInputs = buildTools;
            buildInputs = runtimeLibs;

            dontUseCmakeConfigure = true;
            dontUseMesonConfigure = true;
            enableParallelBuilding = false;

            buildPhase = ''
              runHook preBuild
              cp -r ${thirdParty}/third_party ./third_party
              chmod -R +w third_party
              make linux
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin $out/lib $out/include/nitro
              cp build/libnitro.a $out/lib/
              cp src/*.h $out/include/nitro/
              cp examples/nitroconvert $out/bin/
              for tool in bench/nbench bench/pngverify bench/psdverify bench/pngrobust bench/psdrobust bench/verify bench/robust bench/rgbverify; do
                [ -x "$tool" ] && cp "$tool" $out/bin/
              done
              runHook postInstall
            '';
          };
          
          nitroview-sdl = pkgs.clangStdenv.mkDerivation {
            pname = "nitroview-sdl";
            version = "unstable";
            src = ./.;

            nativeBuildInputs = buildTools ++ (with pkgs; [
              python3
              cmake
              ninja
              meson
              nasm
            ]);

            buildInputs = runtimeLibs ++ (with pkgs; [
              libGLU
              systemdLibs
              libffi
              libxtst
            ]);

            dontUseCmakeConfigure = true;
            dontUseMesonConfigure = true;
            enableParallelBuilding = false;

            buildPhase = ''
              runHook preBuild
              cp -r ${thirdParty}/third_party ./third_party
              chmod -R +w third_party
              make sdl
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin
              cp nitroview-sdl $out/bin/

              runHook postInstall
            '';
          
            postFixup = ''
              patchelf --set-rpath "$(patchelf --print-rpath $out/bin/nitroview-sdl):${pkgs.lib.makeLibraryPath runtimeLibs}:/run/opengl-driver/lib" $out/bin/nitroview-sdl
            '';

            meta = with pkgs.lib; {
              description = "Ultrafast image and vector viewer (SDL3 + OpenGL)";
              homepage = "https://github.com";
              platforms = platforms.linux;
              mainProgram = "nitroview-sdl";
            };
          };

          default = self.packages.${system}.nitroview-sdl;
        });

      devShells = forAllSystems (system:
        let
          env = mkEnv system;
          inherit (env) pkgs isLinux runtimeLibs buildTools;
        in
        {
          default = pkgs.mkShell {
            nativeBuildInputs = buildTools ++ (with pkgs; [
              git
              curl
              python3
              cmake
              ninja
              nasm
              glib.dev
            ]);
            buildInputs = runtimeLibs ++ (with pkgs; [ gtk4 libadwaita ]);

            LD_LIBRARY_PATH = pkgs.lib.optionalString isLinux
              "${pkgs.lib.makeLibraryPath runtimeLibs}:/run/opengl-driver/lib";
            NIX_ENFORCE_NO_NATIVE = "0";
          };
        });
    };
}

