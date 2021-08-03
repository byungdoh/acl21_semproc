# The below targets assume installation of conda dependencies and armadillo as per bin/setup.sh

# Set system variables
export CPATH = $(CONDA_PREFIX)/include
export LD_LIBRARY_PATH = $(CONDA_PREFIX)/lib64
export MKL_NUM_THREADS = 1

# Compile parser binary
.PRECIOUS: bin/semproc
bin/semproc:  src/semproc.cpp  config/cflags.txt
	g++ -L$(CONDA_PREFIX)/lib -L$(CONDA_PREFIX)/lib64 -Iinclude -Wall `cat $(word 2,$^)` -fpermissive -std=c++17 $< -lm -larmadillo -Wl,-rpath-link=$(CONDA_PREFIX)/lib -lpthread -o $@

# Generate by-word surprisal estimates
.PRECIOUS: output/%.charw.surprisal
output/%.charw.surprisal:  bin/semproc  data/%.linetoks  config/parserflags.txt  model/wsj02to21.charw.semprocmodel
	cat $(word 2,$^)  |  $< `cat $(word 3,$^)` $(word 4,$^)  >  $@  2>  $@.log
