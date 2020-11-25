# kff-tools

This repository contains a list of tools that are used to manipulate kff files.
kff file format is [described here](https://github.com/yoann-dufresne/kmer_file_format).

kff-tools is a program containing a set of small programs allowing kff files manipulations.
Each following part describes one of these tools.

## kff-tools validate

Try to read a kff file and validate its structure.
Fail if something is incoherent with the standard but doesn't guaranty the validity if anything seems right.

code status: TODO

## kff-tools split

Split a kff file into one kff file per section.

Parameters:
* **-i &lt;input.kff&gt;** \[required\]: Input file to split.
* **-o &lt;path/&gt;**: Directory where the split output files are written (Default ./).

Usage:
```bash
  kff-tools split -i to_split.kff -o split_dir/
```

Remaining work: The outdir must be created if not exists.

## kff-tools merge

Merge a list of kff files into only one.
The order of the input file will be preserved in the merged output.

Parameters:
* **-i &lt;input1.kff&gt; &lt;input2.kff&gt; ...** \[required\]: Input file list to merge.
All the files must share the same encoding.
If not, please first translate them (you can use the translate subprogram of kff-tools).
* **-o &lt;output.kff&gt;** \[required\]: Name of the merged kff file.

Usage:
```bash
  kff-tools merge -i to_merge_1.kff to_merge_2.kff to_merge_3.kff -o merged.kff
```

## kff-tools outstr

Read a file and print out the kmers and data as strings

code status: TODO

## kff-tools translate

Read and rewrite a kff file changing the nucleotide encoding.

Parameters:
* **-i &lt;input.kff&gt;** \[required\]: File to translate.
* **-o &lt;output.kff&gt;** \[required\]: Translated file.
* **-e &lt;encoding&gt;** \[required\]: 4 chars encoding. All the letters A, C, G and T must be present in the encoding order.
For example, AGTC represent the encoding A=0, G=1, T=2, C=3.

Usage:
```bash
  kff-tools translate -i to_encode.kff -o encoded.kff -e AGTC
```

## kff-tools diff

Read two kff files and print the differences between them

code status: TODO

## kff-tools data-rm

Read a kff file and write the same one with a data size of 0.
i.e. all the data are removed to only keep kmer sequences

code status: TODO

## kff-tools expand

Each block in each section is split into one block per kmer.

code status: TODO
