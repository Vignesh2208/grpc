# Copyright 2017 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load("//bazel:grpc_build_system.bzl", "grpc_cc_binary", "grpc_cc_library", "grpc_cc_test", "grpc_package")

grpc_package(name = "test/core/slice")

licenses(["notice"])  # Apache v2

load("//test/core/util:grpc_fuzzer.bzl", "grpc_fuzzer")

grpc_fuzzer(
    name = "b64_encode_fuzzer",
    srcs = ["b64_encode_fuzzer.cc"],
    corpus = "b64_encode_corpus",
    language = "C++",
    tags = ["no_windows"],
    deps = [
        "//:grpc",
    ],
)

grpc_fuzzer(
    name = "b64_decode_fuzzer",
    srcs = ["b64_decode_fuzzer.cc"],
    corpus = "b64_decode_corpus",
    language = "C++",
    tags = ["no_windows"],
    deps = [
        "//:grpc",
    ],
)

grpc_fuzzer(
    name = "percent_encode_fuzzer",
    srcs = ["percent_encode_fuzzer.cc"],
    corpus = "percent_encode_corpus",
    language = "C++",
    tags = ["no_windows"],
    deps = [
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_fuzzer(
    name = "percent_decode_fuzzer",
    srcs = ["percent_decode_fuzzer.cc"],
    corpus = "percent_decode_corpus",
    language = "C++",
    tags = ["no_windows"],
    deps = [
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "percent_encoding_test",
    srcs = ["percent_encoding_test.cc"],
    language = "C++",
    uses_polling = False,
    deps = [
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "slice_test",
    srcs = ["slice_test.cc"],
    language = "C++",
    uses_polling = False,
    deps = [
        "//:gpr",
        "//:slice",
        "//test/core/util:grpc_suppressions",
    ],
)

grpc_cc_test(
    name = "slice_intern_test",
    srcs = ["slice_intern_test.cc"],
    language = "C++",
    uses_polling = False,
    deps = [
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "slice_string_helpers_test",
    srcs = ["slice_string_helpers_test.cc"],
    language = "C++",
    uses_polling = False,
    deps = [
        "//:gpr",
        "//:slice",
        "//test/core/util:grpc_suppressions",
    ],
)

grpc_cc_test(
    name = "slice_split_test",
    srcs = ["slice_split_test.cc"],
    language = "C++",
    uses_polling = False,
    deps = [
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "slice_buffer_test",
    srcs = ["slice_buffer_test.cc"],
    language = "C++",
    uses_polling = False,
    deps = [
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "b64_test",
    srcs = ["b64_test.cc"],
    language = "C++",
    uses_polling = False,
    deps = [
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)
