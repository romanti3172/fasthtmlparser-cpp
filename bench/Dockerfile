# The Linux benchmark image fhp-bench-cpp, as the images of the other ports (official compiler image, one run).
#
#     docker build -f bench/Dockerfile -t fhp-bench-cpp .
#     docker run --rm fhp-bench-cpp            # the benchmark table
#     docker run --rm fhp-bench-cpp --check    # the records only
#
# lexbor, Gumbo and libxml2 are fetched and built at image build time (CMake FetchContent).
FROM gcc:15

RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake ninja-build git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt LICENSE.md ./
COPY cmake cmake
COPY include include
COPY src src
COPY tests tests
COPY bench bench
COPY testdata testdata

# the tests first: the image is only built from a port that passes them
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build \
    && ./build/tests/fasthtmlparser_tests

RUN cmake -S bench -B bench/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build bench/build \
    && ./bench/build/fasthtmlparser_bench --check

ENTRYPOINT ["/src/bench/build/fasthtmlparser_bench"]
