add_executable(vorbis_test util.c util.h write_read.c write_read.h test.c)
target_link_libraries(vorbis_test PRIVATE Vorbis::vorbisenc $<$<BOOL:${HAVE_LIBM}>:m>)
add_test(NAME vorbis_test COMMAND vorbis_test)

add_executable(test_codebook test_codebook.c)
target_link_libraries(test_codebook PRIVATE vorbis Ogg::ogg $<$<BOOL:${HAVE_LIBM}>:m>)
add_test(NAME test_codebook COMMAND test_codebook)

add_executable(seek_consistency util.c util.h write_read.c write_read.h
  seek_consistency.c ../examples/seek_decode.c ../examples/seek_decode.h)
target_link_libraries(seek_consistency PRIVATE Vorbis::vorbisfile Vorbis::vorbisenc $<$<BOOL:${HAVE_LIBM}>:m>)
add_test(NAME seek_consistency COMMAND seek_consistency)
