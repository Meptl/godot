{
    inputs = {
        nixpkgs.url = "github:nixos/nixpkgs";
        utils.url = "github:numtide/flake-utils";
    };

    outputs = { self, nixpkgs, utils }: (utils.lib.eachDefaultSystem (system:
    let
      pkgs = nixpkgs.legacyPackages."${system}";
    in {
        defaultPackage = let
          options = {
            touch = false;
            pulseaudio = false;
          };
          lib = pkgs.lib;
        in pkgs.ccacheStdenv.mkDerivation rec {
          pname = "godot";
          version = "3.4-dev";

          src = ./.;

          nativeBuildInputs = with pkgs; [ pkg-config ];
          buildInputs = with pkgs; [
            clang
            lld

            scons
            udev
            xorg.libX11
            xorg.libXcursor
            xorg.libXinerama
            xorg.libXrandr
            xorg.libXrender
            xorg.libXi
            xorg.libXext
            xorg.libXfixes
            freetype
            openssl
            alsa-lib
            libpulseaudio
            libGLU
            zlib
            yasm
          ];

          patches = [ ./pkg_config_additions.patch ./dont_clobber_environment.patch ];

          enableParallelBuilding = true;

          sconsFlags = "target=release_debug platform=x11";
          preConfigure = ''
            sconsFlags+=" ${
              lib.concatStringsSep " "
              (lib.mapAttrsToList (k: v: "${k}=${builtins.toJSON v}") options)
            }"
          '';

          outputs = [ "out" "dev" "man" ];

          installPhase = ''
            mkdir -p "$out/bin"
            cp bin/godot.* $out/bin/godot
            cp -r modules/godotsteam/sdk/redistributable_bin/linux64/libsteam_api.so $out/bin/libsteam_api.so
            echo "1567050" > $out/bin/steam_appid.txt

            mkdir "$dev"
            cp -r modules/gdnative/include $dev

            mkdir -p "$man/share/man/man6"
            cp misc/dist/linux/godot.6 "$man/share/man/man6/"

            mkdir -p "$out"/share/{applications,icons/hicolor/scalable/apps}
            cp misc/dist/linux/org.godotengine.Godot.desktop "$out/share/applications/"
            cp icon.svg "$out/share/icons/hicolor/scalable/apps/godot.svg"
            cp icon.png "$out/share/icons/godot.png"
            substituteInPlace "$out/share/applications/org.godotengine.Godot.desktop" \
            --replace "Exec=godot" "Exec=$out/bin/godot"
          '';

          meta = {
            homepage = "https://godotengine.org";
            description = "Free and Open Source 2D and 3D game engine";
            license = lib.licenses.mit;
            platforms = [ "i686-linux" "x86_64-linux" ];
            maintainers = with lib.maintainers; [ twey ];
          };
        };

        defaultApp = utils.lib.mkApp {
          drv = self.defaultPackage."${system}";
        };
      }
      ));
    }
