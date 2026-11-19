Windows 10, AMD Ryzen 7 3800X (VM), MSVC 19.52 (/O2 /Ob2 for every library), lexbor 3.0.1, Gumbo 0.14.1, libxml2 2.15.4, [sanblch/fasthtmlparser-cpp](https://github.com/sanblch/fasthtmlparser-cpp) at 2fb1d7a; `bench/build/fasthtmlparser_bench` (2026-11-19). The median of five batches; a second run was within 3% (books parse within 8%).

"no copy" is `Document::parse_view`; "borrowed" is sanblch's `Document::parse_borrowed` (its own benchmark measures that one). Both C++ ports extract the same records (books: 20 records, FNV d692ad8db738f3c4; quotes: 10 records, FNV a5a9339234866fb0).

| task | library | time | throughput |
|---|---|---:|---:|
| parse quotes 11 KB | fasthtmlparser | 16.1 µs | 705 MB/s |
| parse quotes 11 KB | fasthtmlparser, no copy | 15.9 µs | 713 MB/s |
| parse quotes 11 KB | sanblch/fasthtmlparser-cpp | 20.1 µs | 563 MB/s |
| parse quotes 11 KB | sanblch/fasthtmlparser-cpp, borrowed | 20.3 µs | 559 MB/s |
| parse quotes 11 KB | lexbor | 121.9 µs | 93 MB/s |
| parse quotes 11 KB | Gumbo | 686.6 µs | 17 MB/s |
| parse quotes 11 KB | libxml2 | 311.9 µs | 36 MB/s |
| parse laptops 38 KB | fasthtmlparser | 46.7 µs | 827 MB/s |
| parse laptops 38 KB | fasthtmlparser, no copy | 46.9 µs | 823 MB/s |
| parse laptops 38 KB | sanblch/fasthtmlparser-cpp | 62.7 µs | 617 MB/s |
| parse laptops 38 KB | sanblch/fasthtmlparser-cpp, borrowed | 60.8 µs | 635 MB/s |
| parse laptops 38 KB | lexbor | 365.9 µs | 106 MB/s |
| parse laptops 38 KB | Gumbo | 2056.1 µs | 19 MB/s |
| parse laptops 38 KB | libxml2 | 993.2 µs | 39 MB/s |
| parse books 51 KB | fasthtmlparser | 51.4 µs | 1041 MB/s |
| parse books 51 KB | fasthtmlparser, no copy | 51.0 µs | 1051 MB/s |
| parse books 51 KB | sanblch/fasthtmlparser-cpp | 60.3 µs | 888 MB/s |
| parse books 51 KB | sanblch/fasthtmlparser-cpp, borrowed | 63.3 µs | 846 MB/s |
| parse books 51 KB | lexbor | 344.6 µs | 155 MB/s |
| parse books 51 KB | Gumbo | 2661.3 µs | 20 MB/s |
| parse books 51 KB | libxml2 | 911.2 µs | 59 MB/s |
| parse books x10 510 KB | fasthtmlparser | 542.1 µs | 987 MB/s |
| parse books x10 510 KB | fasthtmlparser, no copy | 537.0 µs | 997 MB/s |
| parse books x10 510 KB | sanblch/fasthtmlparser-cpp | 670.9 µs | 798 MB/s |
| parse books x10 510 KB | sanblch/fasthtmlparser-cpp, borrowed | 635.1 µs | 843 MB/s |
| parse books x10 510 KB | lexbor | 4685.9 µs | 114 MB/s |
| parse books x10 510 KB | Gumbo | 28461.6 µs | 19 MB/s |
| parse books x10 510 KB | libxml2 | 9293.0 µs | 58 MB/s |
| parse books, no </li> </p> | fasthtmlparser | 52.6 µs | 1006 MB/s |
| parse books, no </li> </p> | fasthtmlparser, no copy | 48.7 µs | 1088 MB/s |
| parse books, no </li> </p> | sanblch/fasthtmlparser-cpp | 59.4 µs | 891 MB/s |
| parse books, no </li> </p> | sanblch/fasthtmlparser-cpp, borrowed | 67.0 µs | 790 MB/s |
| parse books, no </li> </p> | lexbor | 330.6 µs | 160 MB/s |
| parse books, no </li> </p> | Gumbo | 2790.7 µs | 19 MB/s |
| parse books, no </li> </p> | libxml2 | 945.6 µs | 56 MB/s |
| parse books 51 KB, pmr arena | fasthtmlparser | 58.4 µs | 917 MB/s |
| parse books 51 KB, no copy | fasthtmlparser | 56.6 µs | 946 MB/s |
| scrape 20 books | fasthtmlparser find | 78.8 µs |  |
| scrape 20 books | fasthtmlparser CSS | 77.7 µs |  |
| scrape 20 books | fasthtmlparser XPath | 128.9 µs |  |
| scrape 20 books | fasthtmlparser find, no copy | 75.1 µs |  |
| scrape 20 books | fasthtmlparser CSS, no copy | 76.9 µs |  |
| scrape 20 books | fasthtmlparser XPath, no copy | 123.2 µs |  |
| scrape 20 books | sanblch/fasthtmlparser-cpp find | 89.9 µs |  |
| scrape 20 books | sanblch/fasthtmlparser-cpp CSS | 95.9 µs |  |
| scrape 20 books | sanblch/fasthtmlparser-cpp XPath | 221.8 µs |  |
| scrape 20 books | sanblch/fasthtmlparser-cpp find, borrowed | 90.4 µs |  |
| scrape 20 books | sanblch/fasthtmlparser-cpp CSS, borrowed | 93.7 µs |  |
| scrape 20 books | sanblch/fasthtmlparser-cpp XPath, borrowed | 209.4 µs |  |
| scrape 20 books | lexbor CSS | 377.0 µs |  |
| scrape 20 books | Gumbo tree walk | 2802.8 µs |  |
| scrape 20 books | libxml2 XPath | 1296.7 µs |  |
| scrape 10 quotes | fasthtmlparser find | 32.0 µs |  |
| scrape 10 quotes | fasthtmlparser CSS | 32.8 µs |  |
| scrape 10 quotes | fasthtmlparser find, no copy | 31.7 µs |  |
| scrape 10 quotes | fasthtmlparser CSS, no copy | 32.6 µs |  |
| scrape 10 quotes | sanblch/fasthtmlparser-cpp find | 38.2 µs |  |
| scrape 10 quotes | sanblch/fasthtmlparser-cpp CSS | 38.8 µs |  |
| scrape 10 quotes | sanblch/fasthtmlparser-cpp find, borrowed | 39.6 µs |  |
| scrape 10 quotes | sanblch/fasthtmlparser-cpp CSS, borrowed | 39.0 µs |  |
| scrape 10 quotes | lexbor CSS | 126.4 µs |  |
| scrape 10 quotes | Gumbo tree walk | 694.1 µs |  |
| scrape 10 quotes | libxml2 XPath | 558.4 µs |  |
