## Introduction
This is the code repository for the paper [Surprisal Estimators for Human Reading Times Need Character Models](https://aclanthology.org/2021.acl-long.290/), including an incremental left-corner parser and a model file trained on WSJ02-21, which can be used to generate surprisal estimates.

Major dependencies include:

- [Miniconda3](https://docs.conda.io/en/master/miniconda.html#)
- [Armadillo](http://arma.sourceforge.net)

## Setup
1) Install [Miniconda3](https://docs.conda.io/en/master/miniconda.html#) and create a new environment.
2) With the new environment activated, run `bin/setup.sh`. This will download and install [Armadillo](http://arma.sourceforge.net) on the environment.
3) Compile the parser using the command `make bin/semproc`.

## Parser Configuration
`config/parserflags.txt` contains two runtime flags that can be modified:
1) `-p`: Number of threads to be used for parallelization. Depending on your computational resources, each thread will parse a different sentence in parallel.
2) `-b`: Width of the beam for beam search. The results reported in [the paper](https://aclanthology.org/2021.acl-long.290/) use a beam width of 5,000.

For example, if you have 20 threads available and would like to use a beam width of 5,000, `config/parserflags.txt` should look like:
```
-p20 -b5000
```

## Surprisal Estimation
The command `make output/myfile.charw.surprisal` can be used to generate by-word surprisal estimates using the parser.
This assumes that the input file `data/myfile.linetoks` is available.
The input file should have one tokenized sentence on each line.
Additionally, the input file is split according to `!ARTICLE` delimiters and assigned to different threads when parallelization is enabled. 

```
$ head data/myfile.linetoks
!ARTICLE
Hello , welcome to our repository .
!ARTICLE
Please refer to the instructions we 've provided in the readme .
```

The output should be a space-delimited two-column file containing the word and its estimated surprisal value.
```
$ head output/myfile.charw.surprisal
word surprisal
Hello 26.4711
, 5.63104
welcome 17.1414
to 10.8348
our 9.86626
repository 32.0564
. 7.41469
```

## Questions
For questions or concerns, please contact Byung-Doh Oh ([oh.531@osu.edu](mailto:oh.531@osu.edu)).