#pragma once

namespace uhdr_repack {

int cli_encode_main(int argc, char** argv);
int cli_dump_gainmap_main(int argc, char** argv);
int cli_inspect_main(int argc, char** argv);

void print_usage();

}  // namespace uhdr_repack
