#include <iostream>

int test_dsp_main();
int test_audio_frame_main();
int test_audio_io_main();
int test_fixed_jitter_buffer_main();
int test_opus_packet_main();
int test_packet_main();
int test_plc_main();
int test_spsc_main();
int test_udp_socket_main();

int main() {
  test_audio_frame_main();
  test_audio_io_main();
  test_fixed_jitter_buffer_main();
  test_opus_packet_main();
  test_packet_main();
  test_udp_socket_main();
  test_spsc_main();
  test_dsp_main();
  test_plc_main();
  std::cout << "udp_audio_tests passed\n";
}
