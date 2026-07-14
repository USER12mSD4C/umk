{ lib, stdenv }:

stdenv.mkDerivation {
  pname = "umk";
  version = "1.0.0";

  src = ./.;

  buildPhase = ''
    runHook preBuild
    $CC -O3 umk.c -o umk
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 umk $out/bin/umk
    runHook postInstall
  '';

  meta = {
    description = "Parallel build system with j-flag support";
    license = lib.licenses.mit;
    platforms = lib.platforms.unix;
    mainProgram = "umk";
  };
}
