#include "mica_server/client.hpp"
#include <exception>
#include <iostream>
int main(int argc, char** argv) {
  try { return mica::client_main(argc, argv); }
  catch (const std::exception& e) { std::cerr << "mica-client: " << e.what() << '\n'; return 1; }
}
