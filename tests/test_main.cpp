#include <iostream>

int test_dsp_main();
int test_packet_main();
int test_plc_main();
int test_spsc_main();

int main() {
  test_packet_main();
  test_spsc_main();
  test_dsp_main();
  test_plc_main();
  std::cout << "udp_audio_tests passed\n";
}

