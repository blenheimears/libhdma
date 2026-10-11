{
  description = "libhdma: standalone C LGPL DCA core and compressed XLL encoder; libsndfile input, Matroska output";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      eachSystem = nixpkgs.lib.genAttrs systems;
    in {
      packages = eachSystem (system:
        let
          pkgs = import nixpkgs { inherit system; };
          sndfileRuntime = "${pkgs.libsndfile.out}/lib/libsndfile.so.1";
          libhdma = pkgs.stdenv.mkDerivation {
            pname = "libhdma";
            version = "0.0.49";
            src = ./.;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.makeWrapper ];
            # FFmpeg is ONLY a test fixture/independent decoder; the target
            # source and linking contain no FFmpeg component whatsoever.
            nativeCheckInputs = [ pkgs.ffmpeg pkgs.python3 ];
            doCheck = true;
            cmakeFlags = [ "-DLIBHDMA_TESTS=ON" "-DLIBHDMA_REQUIRE_FFMPEG_TESTS=ON" ];
            preCheck = ''
              export LIBHDMA_SNDFILE_LIBRARY="${sndfileRuntime}"
              test -f "$LIBHDMA_SNDFILE_LIBRARY" || {
                echo "Missing libsndfile runtime: $LIBHDMA_SNDFILE_LIBRARY" >&2
                exit 1
              }
              command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null
            '';
            postFixup = ''
              wrapProgram "$out/bin/hdmaenc" \
                --set LIBHDMA_SNDFILE_LIBRARY "${sndfileRuntime}"
            '';
            meta = {
              description = "Standalone C DCA core and DCA core plus lossless XLL encoder";
              license = pkgs.lib.licenses.lgpl21Plus;
              platforms = systems;
              mainProgram = "hdmaenc";
            };
          };
        in {
          default = libhdma;
        });
      apps = eachSystem (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/hdmaenc";
        };
      });
      checks = eachSystem (system: {
        encoder = self.packages.${system}.default;
      });
      devShells = eachSystem (system:
        let pkgs = import nixpkgs { inherit system; }; in {
          default = pkgs.mkShell {
            packages = [ pkgs.cmake pkgs.ninja pkgs.ffmpeg pkgs.libsndfile.out pkgs.python3 ];
            shellHook = ''
              export LIBHDMA_SNDFILE_LIBRARY="${pkgs.libsndfile.out}/lib/libsndfile.so.1"
            '';
          };
        });
    };
}
