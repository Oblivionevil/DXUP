#!/bin/bash
# Profile-guided build of DXUP (MSVC, from a shell with its environment): an
# instrumented build, training with the test programs, the final link.
#   bash build_pgo.sh 64|32 [gen|train|use]    default: all three, in build_pgo64 / build_pgo32
# Run the steps one by one to add profiles from games between train and use:
# put pgort140.dll and the instrumented DLLs next to the game and, while it
# runs, `pgosweep d3d11.dll d3d11!game.pgc` in that folder (the same for each
# DLL). EXTRA="folder ..." makes use take the .pgc files from those folders.
# What the training does not reach is compiled for size: without a game's
# profile its API's paths can end up slower than in build_rel.
cd "$(dirname "$0")" || exit 1
BITS=${1:?bits}; STEP=${2:-all}
B=build_pgo$BITS
R=$PWD/artifacts/run_pgo$BITS

if [ "$STEP" = all ] || [ "$STEP" = gen ]; then
  rm -rf "${B:?}"
  MSYS_NO_PATHCONV=1 meson setup "$B" --buildtype=release -Ddebug=true "-Dcpp_args=/GL $PGO_CPP_ARGS" "-Dcpp_link_args=/LTCG /GENPROFILE /OPT:REF /OPT:ICF" $PGO_SETUP_ARGS || exit 1
  ninja -C "$B" || exit 1
fi

if [ "$STEP" = all ] || [ "$STEP" = train ]; then
  rm -rf "${R:?}"
  mkdir -p "$R/d3d9" "$R/dx7" "$R/dx11"
  T=$B/src/tests
  cp -f "$T/d3d9_test_app.exe" "$T/d3d9_shader_test.exe" "$T/d3d8_test_app.exe" "$R/d3d9/"
  cp -f "$B/src/d3d9/d3d9.dll" "$R/d3d9/dxup_d3d9.dll"
  cp -f "$B/src/d3d8/d3d8.dll" "$R/d3d9/dxup_d3d8.dll"
  cp -f "$B/src/d3d11/d3d11.dll" "$B/src/d3d11/dxgi.dll" "$R/d3d9/"
  cp -f "$T/dx7_test_app.exe" "$B/src/ddraw/ddraw.dll" "$B/src/d3d11/d3d11.dll" "$B/src/d3d11/dxgi.dll" "$R/dx7/"
  cp -f "$T/dx11_test_app.exe" "$B/src/d3d11/d3d11.dll" "$B/src/d3d11/dxgi.dll" "$R/dx11/"
  # The profile runtime of this architecture next to each program.
  RT=$(dirname "$(command -v cl)")/pgort140.dll
  [ -f "$RT" ] || RT=$(dirname "$(command -v cl)")/../../Hostx86/x86/pgort140.dll
  for d in d3d9 dx7 dx11; do cp -f "$RT" "$R/$d/" || exit 1; done
  (cd "$R/dx11" && ./dx11_test_app.exe > out.txt 2>&1; grep Summary out.txt
   for i in 1 2 3 4; do
     for f in bound_draw discard_draw submission many_object_bind texture_update; do
       DXUP_TEST_FILTER=$f ./dx11_test_app.exe > /dev/null 2>&1
     done
   done)
  (cd "$R/d3d9" && ./d3d9_test_app.exe > out9.txt 2>&1; grep -E '^[0-9]+ passed' out9.txt | tail -1
   ./d3d9_shader_test.exe > outs.txt 2>&1
   ./d3d9_shader_test.exe --dxbc > outd.txt 2>&1
   ./d3d8_test_app.exe > out8.txt 2>&1)
  (cd "$R/dx7" && ./dx7_test_app.exe > out.txt 2>&1; grep Summary out.txt)
fi

if [ "$STEP" = all ] || [ "$STEP" = use ]; then
  # Every profile next to its .pgd and merged; the link then takes it.
  for pair in d3d11:d3d11 dxgi:d3d11 d3d9:d3d9 d3d8:d3d8 ddraw:ddraw; do
    m=${pair%%:*}; dir=${pair#*:}
    n=0
    while IFS= read -r f; do
      n=$((n+1))
      cp -f "$f" "$B/src/$dir/$m!x$n.pgc"
    done < <(find "$R" $EXTRA -name "$m!*.pgc" 2>/dev/null)
    (cd "$B/src/$dir" && MSYS_NO_PATHCONV=1 pgomgr /merge "$m.pgd" > /dev/null)
    echo "$m: $n profiles"
  done
  # In build.ninja, not through meson: its compiler checks would link with
  # the profile too, which they do not have.
  sed -i 's#/GENPROFILE#/USEPROFILE#g' "$B/build.ninja"
  ninja -C "$B" src/d3d9/d3d9.dll src/d3d8/d3d8.dll src/ddraw/ddraw.dll src/d3d11/d3d11.dll src/d3d11/dxgi.dll || exit 1
fi
