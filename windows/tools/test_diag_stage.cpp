#include "../src/pe/nr_pe_diag_stage.hpp"

#include <cassert>
#include <cstdio>
#include <initializer_list>

int main() {
    assert(nr::pe::diagnostic_stage("0") == 0);
    assert(nr::pe::diagnostic_stage("1") == 1);
    assert(nr::pe::diagnostic_stage("2") == 2);
    assert(nr::pe::diagnostic_stage("3") == 3);
    assert(nr::pe::diagnostic_stage("4") == 4);
    assert(nr::pe::diagnostic_stage("5") == 5);
    assert(nr::pe::diagnostic_stage("6") == 6);
    assert(nr::pe::diagnostic_stage("7") == 7);
    assert(nr::pe::diagnostic_stage("8") == 8);
    for (const char* value : std::initializer_list<const char*>{nullptr, "", "9", "10", "-1", "01", "1x", " 2", "2 ", "3x", "03", "4x", "04", " 4", "4 ", "5x", "05", " 5", "5 ", "6x", "06", " 6", "6 ", "7x", "07", " 7", "7 ", "8x", "08", " 8", "8 ", "normal"})
        assert(nr::pe::diagnostic_stage(value) == 0);
    std::puts("PASS: diagnostic stage 0/1/2/3/4/5/6/7/8; unset, empty and invalid values (including 9+) default to normal");
}
