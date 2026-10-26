# Original FAAD2 sources, deliberately restricted to fixed-point AAC-LC.
faad_lc_sources = files(
  'libfaad/bits.c', 'libfaad/cfft.c', 'libfaad/common.c', 'libfaad/decoder.c',
  'libfaad/drc.c', 'libfaad/error.c', 'libfaad/filtbank.c', 'libfaad/huffman.c',
  'libfaad/is.c', 'libfaad/mdct.c', 'libfaad/mp4.c', 'libfaad/ms.c',
  'libfaad/output.c', 'libfaad/pns.c', 'libfaad/pulse.c', 'libfaad/specrec.c',
  'libfaad/syntax.c', 'libfaad/tns.c',
)
faad_lc_args = ['-DFIXED_POINT', '-DLC_ONLY_DECODER', '-DDISABLE_SBR',
                '-DHAVE_INTTYPES_H=1', '-DHAVE_STRING_H=1',
                '-DHAVE_SYS_TYPES_H=1', '-DHAVE_SYS_STAT_H=1',
                '-DHAVE_STRINGS_H=1', '-DHAVE_UNISTD_H=1', '-DAPPLY_DRC=0',
                '-DPACKAGE_VERSION="2.11.2"']
faad_lc = static_library('db2010-faad-lc', faad_lc_sources,
  files('../db2010-aac-lc.c'), c_args: faad_lc_args,
  include_directories: include_directories('include', 'libfaad', '..'),
  build_by_default: false)
db2010_aac_dependency = declare_dependency(link_with: faad_lc, dependencies: libm)
