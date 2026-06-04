#include "hft_simulator/ui.h"
#include <iostream>

int main(int argc, char** argv) {
    std::cerr << "gui_main: launching GUI" << std::endl;
    int result = hft::run_hft_gui(argc, argv);
    std::cerr << "gui_main: exit code " << result << std::endl;
    return result;
}
