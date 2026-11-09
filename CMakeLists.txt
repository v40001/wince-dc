# wince-dc - self-contained build of the Dreamcast WinCE modules (SH-4) + the bootable
# image. No external SDK: everything is vendored under vendor/. We don't use CMake's
# compiler model (the 1999 Renesas-SH cl.exe isn't a CMake-known compiler), so the
# project language is NONE and every step is an explicit custom command driving the
# vendored cl.exe / shasm.exe / link.exe and the makeimg image pipeline.
cmake_minimum_required(VERSION 3.20)
project(wince_dc NONE)

set(REPO       ${CMAKE_CURRENT_SOURCE_DIR})
set(SH_BIN     ${REPO}/vendor/sh-toolchain/bin/I386/SH)
set(HOST_BIN   ${REPO}/vendor/sh-toolchain/bin/I386)
set(CL         ${SH_BIN}/cl.exe)
set(SHASM      ${SH_BIN}/shasm.exe)
set(LINK       ${HOST_BIN}/link.exe)
set(SDK        ${REPO}/vendor/wcesdk)
set(SDK_INC    ${SDK}/inc)
set(SDK_LIB    ${SDK}/lib/retail)
set(CE3_ASM_INC ${REPO}/vendor/sh-toolchain/ce3-ppc2k/include)
set(OUTDIR     ${CMAKE_BINARY_DIR}/modules)         # built .dll/.exe land here
file(MAKE_DIRECTORY ${OUTDIR})

# SH-4 CE compile flags (same as the old build-*.bat CF).
set(CFLAGS /nologo /c /W3 -DUNDER_CE=212 -D_WIN32_WCE=212 -DUNICODE -D_UNICODE -DSH4=1 -DSHx=1)

# cl.exe/link.exe load their backend DLLs (c1*.dll, mspdb*.dll, ...) from the toolchain
# bins via PATH (the old .bats put SH_BIN;HOST_BIN on PATH). Prepend them per-command via
# `cmake -E env --modify path_list_prepend` (handles the ';' correctly, unlike a raw env).
set(ENVPFX ${CMAKE_COMMAND} -E env
    --modify "PATH=path_list_prepend:${SH_BIN}"
    --modify "PATH=path_list_prepend:${HOST_BIN}" --)

# add_sh_module(NAME TYPE dll|exe ENTRY <sym> [DEF <file>] OUT <file>
#               SOURCES <c...> [ASM <src...>] [LIBS <name.lib...>] [INCLUDES <dir...>])
# Includes are passed as /I (no INCLUDE env), libs as full paths (no LIB env), so there
# are no semicolon-list pitfalls; cl/link find their backend DLLs next to themselves.
function(add_sh_module NAME)
  cmake_parse_arguments(M "" "TYPE;ENTRY;DEF;OUT" "SOURCES;ASM;LIBS;INCLUDES;EXTRALIBS" ${ARGN})
  set(objs "")
  set(incflags /I${SDK_INC})
  foreach(d ${M_INCLUDES})
    list(APPEND incflags /I${d})
  endforeach()
  foreach(src ${M_SOURCES})
    get_filename_component(base ${src} NAME_WE)
    set(obj ${OUTDIR}/${NAME}_${base}.obj)
    add_custom_command(OUTPUT ${obj}
      COMMAND ${ENVPFX} "${CL}" ${CFLAGS} ${incflags} /Fo${obj} ${src}
      DEPENDS ${src} VERBATIM)
    list(APPEND objs ${obj})
  endforeach()
  foreach(src ${M_ASM})
    get_filename_component(base ${src} NAME_WE)
    set(obj ${OUTDIR}/${NAME}_${base}.obj)
    add_custom_command(OUTPUT ${obj}
      COMMAND ${ENVPFX} "${SHASM}" -cpu=SH4 -DSH4=1 -DSHx=1 -nologo -I${CE3_ASM_INC} -object=${obj} ${src}
      DEPENDS ${src} VERBATIM)
    list(APPEND objs ${obj})
  endforeach()
  set(out ${OUTDIR}/${M_OUT})
  set(lf /nologo /machine:SH4 /subsystem:windowsce,2.12 /entry:${M_ENTRY})
  set(byprod "")
  if(M_TYPE STREQUAL "dll")
    list(APPEND lf /dll)
    get_filename_component(_ob ${M_OUT} NAME_WE)   # link /dll also emits the import lib + exp
    set(byprod BYPRODUCTS ${OUTDIR}/${_ob}.lib ${OUTDIR}/${_ob}.exp)
  endif()
  if(M_DEF)
    list(APPEND lf /def:${M_DEF})
  endif()
  set(libs "")
  foreach(l ${M_LIBS})
    list(APPEND libs ${SDK_LIB}/${l})
  endforeach()
  foreach(l ${M_EXTRALIBS})      # full-path libs built by us (e.g. the generated dcspi.lib)
    list(APPEND libs ${l})
  endforeach()
  add_custom_command(OUTPUT ${out}
    COMMAND ${ENVPFX} "${LINK}" ${lf} /out:${out} ${objs} ${libs}
    ${byprod}
    DEPENDS ${objs} ${M_EXTRALIBS} VERBATIM)
  add_custom_target(${NAME} ALL DEPENDS ${out})
  set_property(GLOBAL APPEND PROPERTY WINCE_MODULES ${out})
endfunction()

# ---- SPI transport ----------------------------------------------------------------
add_sh_module(dcspi TYPE dll ENTRY DllMain DEF ${REPO}/drivers/dcspi/dcspi.def OUT dcspi.dll
  SOURCES ${REPO}/drivers/dcspi/dcspi.c
  LIBS coredll.lib corelibc.lib)

# ---- SD-card storage: SD block driver + ChaN FatFs (WDM/AFS mount layer is WIP) ----
add_sh_module(sdcard TYPE dll ENTRY DllMain OUT sdcard.dll
  SOURCES ${REPO}/drivers/sdcard/sdcard.c ${REPO}/drivers/sdcard/sdblk.c
          ${REPO}/drivers/sdcard/diskio.c ${REPO}/shell/syslog.c ${REPO}/shell/dcboot.c
          ${REPO}/drivers/sdcard/fatfs/ff.c ${REPO}/drivers/sdcard/fatfs/ccsbcs.c
  INCLUDES ${REPO}/drivers/sdcard ${REPO}/drivers/sdcard/fatfs ${REPO}/drivers/dcspi ${REPO}/shell
  LIBS coredll.lib corelibc.lib wdm.lib
  EXTRALIBS ${OUTDIR}/dcspi.lib)
add_dependencies(sdcard dcspi)

# ---- universal microstk link shim (mppp.dll) --------------------------------------
add_sh_module(mppp TYPE dll ENTRY dllentry DEF ${REPO}/net/netif/netif.def OUT mppp.dll
  SOURCES ${REPO}/net/netif/netif.c ${REPO}/net/netif/bba_hw.c ${REPO}/net/netif/ras.c
          ${REPO}/net/netif/w5500.c ${REPO}/net/netif/flashrom.c
          ${REPO}/shell/syslog.c ${REPO}/shell/dcboot.c
  ASM     ${REPO}/net/netif/g2lock.src
  INCLUDES ${REPO}/drivers/dcspi ${REPO}/shell
  LIBS coredll.lib corelibc.lib)

# ---- DCWin desktop shell + PVR2 compositor ----------------------------------------
add_sh_module(dcshell TYPE exe ENTRY WinMainCRTStartup OUT dcshell.exe
  SOURCES ${REPO}/shell/dcshell.c ${REPO}/shell/dcgfx.c ${REPO}/shell/dcinput.c ${REPO}/shell/vmustore.c
  LIBS coredll.lib corelibc.lib ddraw.lib d3dim.lib dinput.lib dxguid.lib mapledev.lib psguid.lib wdm.lib)

# ---- DCWin client apps (each links the client lib) --------------------------------
foreach(app dcwcalc dcwclock dcwexp dcwtask dcwmem dcwcmd dcwcpl dcwhelp
        dcwmail dcwpword dcwterm)
  add_sh_module(${app} TYPE exe ENTRY WinMainCRTStartup OUT ${app}.exe
    SOURCES ${REPO}/shell/${app}.c ${REPO}/shell/dcwlib.c
    LIBS coredll.lib corelibc.lib)
endforeach()
add_sh_module(dcwnet TYPE exe ENTRY WinMainCRTStartup OUT dcwnet.exe
  SOURCES ${REPO}/shell/dcwnet.c ${REPO}/shell/dcwlib.c
  LIBS coredll.lib corelibc.lib winsock.lib mras.lib)
add_sh_module(dcwlog TYPE exe ENTRY WinMainCRTStartup OUT dcwlog.exe   # the System Log viewer
  SOURCES ${REPO}/shell/dcwlog.c ${REPO}/shell/dcwlib.c ${REPO}/shell/syslog.c
  LIBS coredll.lib corelibc.lib)

# ---- Internet Explorer: fullscreen Trident WebBrowser host -------------------------
# NOT a dcw* app: the shell launches it via the display hand-off (GfxLaunch), so it owns
# the GWES display and hosts the stock WebBrowser control. Needs IMGHTML=1 in the image.
add_sh_module(iexplore TYPE exe ENTRY WinMainCRTStartup OUT iexplore.exe
  SOURCES ${REPO}/shell/iexplore.cpp ${REPO}/shell/iehost.cpp ${REPO}/shell/ieproto.cpp
          ${REPO}/shell/dcgfx.c ${REPO}/shell/dcinput.c
  LIBS coredll.lib corelibc.lib ole32.lib oleaut32.lib uuid.lib winsock.lib urlmon.lib
       ddraw.lib d3dim.lib dinput.lib dxguid.lib mapledev.lib psguid.lib wdm.lib)

# ---- boot loader (autorun): NT-style menu (Normal / Internet Explorer), then boots the choice ---
add_sh_module(dcwboot TYPE exe ENTRY WinMainCRTStartup OUT dcwboot.exe
  SOURCES ${REPO}/shell/dcwboot.c ${REPO}/shell/dcgfx.c ${REPO}/shell/dcinput.c
  ASM     ${REPO}/shell/trampoline.src
  LIBS coredll.lib corelibc.lib ddraw.lib d3dim.lib dinput.lib dxguid.lib mapledev.lib psguid.lib
       winsock.lib wdm.lib)

# ---- music player: WAV + MP3 (minimp3), DirectSound, spectrum viz, clickable UI ----
# Built but NOT baked into the ROM (no platform.bib entry) - keep it compiling without bloating
# the networking image; re-add the dcwplay.exe line to platform.bib to ship it.
add_sh_module(dcwplay TYPE exe ENTRY WinMainCRTStartup OUT dcwplay.exe
  SOURCES ${REPO}/shell/dcwplay.c ${REPO}/shell/dcwlib.c ${REPO}/shell/mp3impl.cpp
  INCLUDES ${REPO}/vendor/minimp3 ${REPO}/shell
  LIBS coredll.lib corelibc.lib dsound.lib)

# ====================================================================================
# Bootable image pipeline (opt-in: `cmake --build build --target gdi`).
#   modules -> deploy into image/OS -> makeimg -> NK.bin -> wrap -> 0winceos.bin -> disc
# The read-only vendored release tree (vendor/wcesdk/image) is seeded into a writable
# build work-dir; our freshly-built modules overlay image/OS before makeimg runs.
#
# Configurable:
#   -DKERNEL=retail|debug   retail = silent nknodbg.exe; debug = patched (no-KD) SCIF-console
#                           nkscifkd.exe + the checked DLL overlay (vendor/wcesdk/image-debug).
#   -DAUTORUN=<exe>         program baked into HKLM\init "Autorun" (default dcshell.exe).
# ====================================================================================
# Kernel and the system-DLL set are independent knobs:
#   -DKERNEL=retail|debug  retail = silent nknodbg.exe; debug = patched (no-KD) SCIF-console
#                          nkscifkd.exe. The kernel only changes boot logging.
#   -DDLLS=retail|debug    retail (default) = the stock retail OS DLLs (what real games run on);
#                          debug = overlay the checked DLLs from vendor/wcesdk/image-debug.
# So a SCIF console over the working retail userland is `-DKERNEL=debug` (DLLS stays retail);
# the checked DLLs are opt-in (they assert/break some titles, e.g. DDHAL under DirectDraw).
set(KERNEL  "retail"       CACHE STRING "image kernel: retail (silent nknodbg) or debug (SCIF-console nkscifkd)")
set(IEKERNEL "${KERNEL}"   CACHE STRING "IE/HTML image kernel: retail or debug (SCIF). Defaults to KERNEL; set debug for a SCIF console in the chainloaded IE image while testing the browser")
set(DLLS    "retail"       CACHE STRING "system DLLs: retail (stock) or debug (checked overlay)")
set(AUTORUN "dcwboot.exe"  CACHE STRING "program baked into the image's HKLM\\init Autorun value (default dcwboot.exe = the NT-style boot menu that can chainload the IE image; use dcshell.exe to boot straight to the desktop)")
set_property(CACHE KERNEL   PROPERTY STRINGS retail debug)
set_property(CACHE IEKERNEL PROPERTY STRINGS retail debug)
set_property(CACHE DLLS   PROPERTY STRINGS retail debug)

if(KERNEL STREQUAL "debug")
  set(KERNEL_IMGFLAG "IMGSERIALDBG=1")            # fmerge picks the patched nkscifkd.exe
else()
  set(KERNEL_IMGFLAG "IMGNODEBUGGER=1")           # fmerge picks the silent nknodbg.exe
endif()
if(IEKERNEL STREQUAL "debug")
  set(IEKERNEL_IMGFLAG "IMGSERIALDBG=1")
else()
  set(IEKERNEL_IMGFLAG "IMGNODEBUGGER=1")
endif()
if(DLLS STREQUAL "debug")
  set(IMG_DEBUG_DIR "${SDK}/image-debug")         # overlay checked coredll/gwes/microstk/...
else()
  set(IMG_DEBUG_DIR "none")
endif()
message(STATUS "image: KERNEL=${KERNEL} IEKERNEL=${IEKERNEL} DLLS=${DLLS} AUTORUN=${AUTORUN}")

set(IMG_SEED ${SDK}/image)
set(IMG_WORK ${CMAKE_BINARY_DIR}/image)
set(NKBIN    ${IMG_WORK}/NK.bin)
set(WINCEOS  ${CMAKE_BINARY_DIR}/0winceos.bin)
set(DISCDIR  ${CMAKE_BINARY_DIR}/disc)

get_property(MODS GLOBAL PROPERTY WINCE_MODULES)
string(REPLACE ";" "|" MODS_BAR "${MODS}")        # '|'-join so -DMODS survives as one arg

# The 1999 romimage/fmerge can't parse forward-slash paths (it mangles the path join),
# so feed it native backslash paths for the env values.
file(TO_NATIVE_PATH "${IMG_WORK}" IMG_WORK_NAT)
file(TO_NATIVE_PATH "${SDK}"      SDK_NAT)

# makeimg environment (no PATH here; the tools dir is prepended separately via --modify so
# the ';' stays intact). KERNEL_IMGFLAG selects which kernel fmerge bakes in.
set(IMG_ENV
  "CEVersion=212" "CESubsystem=windowsce,2.12" "CEConfigName=Dreamcast"
  # forward slashes on purpose: a trailing '\' would escape the closing quote on the
  # command line and blank the var (romimage then collapses every path to flat). Win32
  # accepts mixed separators, so $(_FLATRELEASEDIR)/OS/foo.dll resolves fine.
  # IMGHTML core split: IE (shdocvw/mshtml) needs the FULL CoreOS_HTML coredll/gwes/ole32 (it
  # imports GDI/USER/OLE by ordinal that the Standard "game" CoreOS doesn't export). But filesys
  # must stay Standard - the HTML filesys.exe faults on our patched (Standard) kernel and the
  # CD-ROM never mounts. Standard filesys runs on the HTML coredll (export superset). The open
  # risk this tests on HW: whether the HTML coredll/gwes run on the Standard patched kernel.
  "RELEASEDIR_STDCORE=/CoreOS_Standard/" "RELEASEDIR_HTMLCORE=/CoreOS_HTML/"
  "RELEASEDIR_HTMLFS=/CoreOS_Standard/"   # filesys only: kernel-matched for the CD-ROM mount
  "RELEASEDIR_OS=/OS/" "RELEASEDIR_SAMPLES=/Samples/" "RELEASEDIR_APPS=/Applications/"
  "${KERNEL_IMGFLAG}" "IMGNOSHELL=1" "IMGDIRECTDRAW=1" "IMGDIRECT3DIM=1" "IMGDDHAL=1"
  "IMGDIRECTSOUND=1" "IMGDIRECTINPUT=1" "IMGMICROSTK=1" "IMGNOCOMM=1" "IMGNOPCMCIA=1"
  "IMGNOCEDDK=1" "INITNOCOMM=1" "INITMICROSTK=1" "INITSERIAL=1" "INITMODEM=1"
  "INITDIRECTSHOW=1" "COUNTRY=USA" "_HOSTCPUTYPE=i386" "_TGTPROJ=DRAGON"
  "_DEPTREES=winceos gemini mcputech dshowdm6"
  "WCEDREAMCASTROOT=${SDK_NAT}" "_PUBLICROOT=${SDK_NAT}" "_FLATRELEASEDIR=${IMG_WORK_NAT}")

# IMGHTML bakes the Trident/IE stack (mshtml/shdocvw/wininet/urlmon/jscript/schannel/imgutil/
# pngfilt/mlang/oleaut32 + their gemini.reg COM registration) into NK.bin, for iexplore.exe.
# It pulls ~3.3 MB + a large registry; toggle off (-DHTML=off) to get the lean image back.
option(HTML "bake the Trident/IE stack (IMGHTML) for the browser" OFF)
if(HTML)
  list(APPEND IMG_ENV "IMGHTML=1")
endif()
message(STATUS "image: HTML(IMGHTML)=${HTML}")

# Prep the work-dir (seed + optional debug overlay + our modules + Autorun) then makeimg.
add_custom_command(OUTPUT ${NKBIN}
  COMMAND ${CMAKE_COMMAND} -DSEED=${IMG_SEED} -DWORK=${IMG_WORK} -DDEBUG_DIR=${IMG_DEBUG_DIR}
          -DAUTORUN=${AUTORUN} -DMODS=${MODS_BAR} -DASSETS=${REPO}/shell/assets
          -P ${REPO}/cmake/prep-image.cmake
  COMMAND ${CMAKE_COMMAND} -E env --modify "PATH=path_list_prepend:${SDK}/tools"
          ${IMG_ENV} -- "${SDK}/tools/makeimg.exe"
  WORKING_DIRECTORY ${IMG_WORK}
  DEPENDS ${MODS} ${REPO}/cmake/prep-image.cmake VERBATIM
  COMMENT "makeimg: building ${KERNEL} CE ROM (NK.bin)")

# Wrap NK.bin into the bootable 0winceos.bin (plants the SH-4 full-MMU magic word).
add_custom_command(OUTPUT ${WINCEOS}
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${REPO}/toolchain/wrap-image.ps1
          -NkBin ${NKBIN} -Out ${WINCEOS} -DcSdk ${SDK}
  DEPENDS ${NKBIN} VERBATIM
  COMMENT "wrap-image: NK.bin -> 0winceos.bin")

add_custom_target(image DEPENDS ${WINCEOS})

# ---- second image: the IMGHTML (Trident/IE) ROM, built by default so the disc can chainload it ---
# dcwboot's boot menu chainloads this 0WINCEOSHTML.BIN over the running kernel (the IE/Trident stack
# bloats the ROM ~6 MB, too big to vendor in the lean main image). It's a full IMGHTML build with
# iexplore.exe baked into ROM (platform.bib IF IMGHTML) and autorun = dcshell.exe (so it boots
# straight to the desktop, where dcshell shows the IE icon). Same kernel/DLL knobs as the main image.
# Toggle off (-DIEIMAGE=off) for a fast lean-only build with no browser.
option(IEIMAGE "also build the IMGHTML/IE image (0winceoshtml.bin) for dcwboot to chainload" ON)
if(IEIMAGE)
  set(IMG_WORK_HTML ${CMAKE_BINARY_DIR}/image-html)
  set(NKBIN_HTML    ${IMG_WORK_HTML}/NK.bin)
  set(WINCEOSHTML   ${CMAKE_BINARY_DIR}/0winceoshtml.bin)
  file(TO_NATIVE_PATH "${IMG_WORK_HTML}" IMG_WORK_HTML_NAT)
  # Same env as the main image but pointed at the HTML work-dir + IMGHTML on.
  set(IMG_ENV_HTML ${IMG_ENV})
  list(REMOVE_ITEM IMG_ENV_HTML "_FLATRELEASEDIR=${IMG_WORK_NAT}")
  list(APPEND IMG_ENV_HTML "_FLATRELEASEDIR=${IMG_WORK_HTML_NAT}" "IMGHTML=1")
  # The IE image can run a different kernel than the main image (IEKERNEL) - e.g. main retail +
  # IE SCIF for browser testing. Swap the kernel fmerge flag if they differ.
  if(NOT IEKERNEL STREQUAL KERNEL)
    list(REMOVE_ITEM IMG_ENV_HTML "${KERNEL_IMGFLAG}")
    list(APPEND IMG_ENV_HTML "${IEKERNEL_IMGFLAG}")
  endif()
  # DEPENDS ${NKBIN} serialises the two makeimg runs (they share SDK tool temp state).
  add_custom_command(OUTPUT ${NKBIN_HTML}
    COMMAND ${CMAKE_COMMAND} -DSEED=${IMG_SEED} -DWORK=${IMG_WORK_HTML} -DDEBUG_DIR=${IMG_DEBUG_DIR}
            -DAUTORUN=dcshell.exe -DMODS=${MODS_BAR} -DASSETS=${REPO}/shell/assets
            -P ${REPO}/cmake/prep-image.cmake
    COMMAND ${CMAKE_COMMAND} -E env --modify "PATH=path_list_prepend:${SDK}/tools"
            ${IMG_ENV_HTML} -- "${SDK}/tools/makeimg.exe"
    WORKING_DIRECTORY ${IMG_WORK_HTML}
    DEPENDS ${MODS} ${NKBIN} ${REPO}/cmake/prep-image.cmake VERBATIM
    COMMENT "makeimg: building the IMGHTML/IE CE ROM (NK.bin)")
  add_custom_command(OUTPUT ${WINCEOSHTML}
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${REPO}/toolchain/wrap-image.ps1
            -NkBin ${NKBIN_HTML} -Out ${WINCEOSHTML} -DcSdk ${SDK}
    DEPENDS ${NKBIN_HTML} VERBATIM
    COMMENT "wrap-image: IE NK.bin -> 0winceoshtml.bin")
  add_custom_target(ieimage DEPENDS ${WINCEOSHTML})
  add_dependencies(ieimage iexplore)
endif()

# Build the bootable GDI (Half-Life DC pipeline: buildgdi + ip.bin) -> disc.gdi.
#   -DEXTRADATA=<dir>  optional folder whose contents go into the disc's \CD-ROM
#                      (e.g. a game's files; our 0winceos.bin always overrides 0WINCEOS.BIN).
set(EXTRADATA "extradata" CACHE STRING "folder whose contents go into the disc's \\CD-ROM (relative paths resolve against the repo root; default: the repo's extradata/ drop folder)")
# \CD-ROM staging dir. We keep some files OUT of the lean ROM and ship them on the disc instead:
# dcwplay.exe (the self-contained media player) always; the IMGHTML/IE image (0WINCEOSHTML.BIN) when
# IEIMAGE is on, so dcwboot can chainload it; plus everything in the EXTRADATA drop folder. (iexplore
# itself is NOT staged here - it lives in the IE image's ROM, since that image is chainloaded from an
# arbitrary disc and can't rely on \CD-ROM.)
set(CDROM ${CMAKE_BINARY_DIR}/cdrom)
set(GDI_STAGE
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CDROM}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CDROM}
  COMMAND ${CMAKE_COMMAND} -E copy ${OUTDIR}/dcwplay.exe ${CDROM}/dcwplay.exe)
if(IEIMAGE)
  list(APPEND GDI_STAGE COMMAND ${CMAKE_COMMAND} -E copy ${WINCEOSHTML} ${CDROM}/0WINCEOSHTML.BIN)
endif()
if(EXTRADATA)
  get_filename_component(EXTRADATA_ABS "${EXTRADATA}" ABSOLUTE BASE_DIR "${REPO}")
  if(EXISTS "${EXTRADATA_ABS}")
    list(APPEND GDI_STAGE COMMAND ${CMAKE_COMMAND} -E copy_directory "${EXTRADATA_ABS}" ${CDROM})
    message(STATUS "gdi: EXTRADATA=${EXTRADATA_ABS}")
  endif()
endif()
add_custom_target(gdi
  ${GDI_STAGE}
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${REPO}/toolchain/make-gdi.ps1
          -Image ${WINCEOS} -OutDir ${DISCDIR} -ExtraData ${CDROM}
  DEPENDS ${WINCEOS}
  COMMENT "make-gdi: 0winceos.bin (+ disc apps) -> disc.gdi"
  USES_TERMINAL)
add_dependencies(gdi dcwplay)
if(IEIMAGE)
  add_dependencies(gdi ieimage)
endif()

# Same pipeline as `gdi` but with a FULL, contiguously-padded data track (-Full -> ~1.1 GB, no
# sparse gap). Slower/larger; use for GDEMU / loaders that dislike the gapped multi-track layout.
add_custom_target(gdi-full
  ${GDI_STAGE}
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${REPO}/toolchain/make-gdi.ps1
          -Image ${WINCEOS} -OutDir ${DISCDIR} -ExtraData ${CDROM} -Full
  DEPENDS ${WINCEOS}
  COMMENT "make-gdi (FULL, no truncate): 0winceos.bin (+ disc apps) -> disc.gdi"
  USES_TERMINAL)
add_dependencies(gdi-full dcwplay)
if(IEIMAGE)
  add_dependencies(gdi-full ieimage)
endif()
