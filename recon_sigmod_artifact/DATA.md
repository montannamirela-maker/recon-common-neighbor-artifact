# Dataset Notes

The paper uses public web and social graph datasets. This artifact does not
redistribute large raw datasets. Reviewers can obtain the datasets from their
original public sources and convert them to Matrix Market or plain edge-list
format.

## Input Format

CBCN accepts either Matrix Market style files or plain edge lists.

Matrix Market example:

```text
%%MatrixMarket matrix coordinate pattern symmetric
6 6 8
1 2
1 3
2 3
...
```

Plain edge-list example:

```text
0 1
0 2
1 2
```

The implementation removes duplicate neighbors after loading and stores sorted
adjacency lists internally.

## Dataset Families Used in the Paper

The experiments use public web graph datasets including small web graphs
(`web-polblogs`, `web-google`, `web-edu`, `web-EPA`, `web-BerkStan`,
`web-webbase-2001`) and larger web graphs (`web-indochina-2004`,
`web-wikipedia2009`, `web-it-2004`, `web-sk-2005`). The paper also compares
against publicly available implementations or reproductions of interval-based
and rule-based compressed graph representations.

For large graphs, the expected workflow is:

1. Download the raw graph from its public source.
2. Convert to Matrix Market or edge-list format.
3. Run CBCN with an appropriate partition bound and overlap threshold.
4. Compare the generated CSV summaries against the processed result files in
   `results/`.

## Toy Dataset

`data/toy/toy_graph.mtx` is included only for a quick correctness and build
check. It is not used for paper claims.

