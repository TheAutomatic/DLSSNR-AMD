#include "../src/pe/nr_pe_diag_stage.hpp"

#include <cassert>
#include <cstdio>
#include <initializer_list>

int main() {
    assert(nr::pe::diagnostic_stage("0") == 0);
    assert(nr::pe::diagnostic_stage("1") == 1);
    assert(nr::pe::diagnostic_stage("2") == 2);
    assert(nr::pe::diagnostic_stage("3") == 3);
    for (const char* value : std::initializer_list<const char*>{nullptr, "", "4", "5", "-1", "01", "1x", " 2", "2 ", "3x", "03", "normal"})
        assert(nr::pe::diagnostic_stage(value) == 0);
    std::puts("PASS: diagnostic stage 0/1/2/3; unset, empty and invalid values (including 4/5) default to normal");
}
