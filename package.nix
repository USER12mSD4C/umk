{ lib, stdenv }:

stdenv.mkDerivation {
  pname = "umk";
  version = "1.0.0";

  src = ./.;

  buildPhase = ''
    runHook preBuild
    $CC -O3 -std=c11 umk.c -o umk
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 umk $out/bin/umk
    install -Dm644 umk.1 $out/share/man/man1/umk.1
    runHook postInstall
  '';

  meta = {
    description = "Simple build system with content hash caching";
    license = lib.licenses.mit;
    platforms = lib.platforms.unix;
    mainProgram = "umk";
  };
}
