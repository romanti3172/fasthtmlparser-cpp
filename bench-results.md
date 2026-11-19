Windows 10, AMD Ryzen 7 3800X (VM), MSVC 19.52 (/O2), lexbor 3.0.1, Gumbo 0.14.1, libxml2 2.15.4; `bench/build/fasthtmlparser_bench` (2026-11-19). The median of five batches; a second run was within 3%.

| task | library | time | throughput |
|---|---|---:|---:|
| parse quotes 11 KB | fasthtmlparser | 16.4 µs | 690 MB/s |
| parse quotes 11 KB | lexbor | 114.0 µs | 100 MB/s |
| parse quotes 11 KB | Gumbo | 695.4 µs | 16 MB/s |
| parse quotes 11 KB | libxml2 | 310.4 µs | 37 MB/s |
| parse laptops 38 KB | fasthtmlparser | 49.0 µs | 789 MB/s |
| parse laptops 38 KB | lexbor | 362.9 µs | 106 MB/s |
| parse laptops 38 KB | Gumbo | 2113.4 µs | 18 MB/s |
| parse laptops 38 KB | libxml2 | 992.2 µs | 39 MB/s |
| parse books 51 KB | fasthtmlparser | 56.8 µs | 942 MB/s |
| parse books 51 KB | lexbor | 336.1 µs | 159 MB/s |
| parse books 51 KB | Gumbo | 2833.2 µs | 19 MB/s |
| parse books 51 KB | libxml2 | 957.6 µs | 56 MB/s |
| parse books x10 510 KB | fasthtmlparser | 565.7 µs | 946 MB/s |
| parse books x10 510 KB | lexbor | 4632.7 µs | 116 MB/s |
| parse books x10 510 KB | Gumbo | 28641.5 µs | 19 MB/s |
| parse books x10 510 KB | libxml2 | 9097.5 µs | 59 MB/s |
| parse books, no </li> </p> | fasthtmlparser | 53.6 µs | 987 MB/s |
| parse books, no </li> </p> | lexbor | 300.9 µs | 176 MB/s |
| parse books, no </li> </p> | Gumbo | 2730.8 µs | 19 MB/s |
| parse books, no </li> </p> | libxml2 | 920.0 µs | 58 MB/s |
| parse books 51 KB, pmr arena | fasthtmlparser | 56.1 µs | 954 MB/s |
| parse books 51 KB, no copy | fasthtmlparser | 55.7 µs | 961 MB/s |
| scrape 20 books | fasthtmlparser find | 80.2 µs |  |
| scrape 20 books | fasthtmlparser CSS | 79.6 µs |  |
| scrape 20 books | fasthtmlparser XPath | 127.4 µs |  |
| scrape 20 books | lexbor CSS | 376.8 µs |  |
| scrape 20 books | Gumbo tree walk | 2848.6 µs |  |
| scrape 20 books | libxml2 XPath | 1241.1 µs |  |
| scrape 10 quotes | fasthtmlparser find | 37.3 µs |  |
| scrape 10 quotes | fasthtmlparser CSS | 39.3 µs |  |
| scrape 10 quotes | lexbor CSS | 129.1 µs |  |
| scrape 10 quotes | Gumbo tree walk | 673.7 µs |  |
| scrape 10 quotes | libxml2 XPath | 526.6 µs |  |
