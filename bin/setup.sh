## assumes that a separate conda environment has been created and activated (e.g. using the commands below)
# conda create -n semproc
# conda activate semproc

# install compilers/BLAS dependencies (for armadillo)
# conda-forge needs to assume high priority for correct conda dependencies
conda config --add channels conda-forge
conda config --set channel_priority strict
conda install -c conda-forge compilers libblas liblapack arpack superlu mkl

# download armadillo from source
wget http://sourceforge.net/projects/arma/files/armadillo-9.900.3.tar.xz
tar -xJf armadillo-9.900.3.tar.xz

# install armadillo to conda env, using mkl as primary BLAS
cd armadillo-9.900.3
cmake -D DETECT_HDF5=false . -DCMAKE_INSTALL_PREFIX:PATH=$CONDA_PREFIX
make
make install
cd ..
