#pragma once

namespace uhdr_repack {

struct EncodeRequest;

/**
 * Consume the encode option at argv[*i] (and its value) into req, advancing *i.
 * Returns 1 consumed, 0 not an encode option, -1 bad value (message on stderr).
 */
int parse_encode_flag(int argc, char** argv, int* i, EncodeRequest* req);

int cli_encode_main(int argc, char** argv);
int cli_dump_gainmap_main(int argc, char** argv);
int cli_inspect_main(int argc, char** argv);
int describe_input_main(int argc, char** argv);

void print_usage();

}  // namespace uhdr_repack
