# daoTools [![daoTools](https://github.com/Durham-Adaptive-Optics/daoTools/actions/workflows/main.yml/badge.svg?branch=CI-Workflow)](https://github.com/Durham-Adaptive-Optics/daoTools/actions/workflows/main.yml)
Useful tools using daoBase

# Prerequiries
## daoBase
daoBase should be installed. See https://github.com/Durham-Adaptive-Optics/daoBase

## other dependencies (CLI11+cfitsio+libyaml-cpp+blas+fmt+zmq+protobuf)
````
sudo apt install libcli11-dev libcfitsio-dev libyaml-cpp-dev libopenblas-dev libfmt-dev libzmq-dev libprotobuf-dev protobuf-compiler
````

## ncurses
on centos:
````
sudo yum install ncurses-devel
````

We recommand to use magicPlot
Some of our plot tool uses magicPlot (optional)
```
pip install magicPlot
```

# Build
```
waf configure --prefix=$DAOROOT
waf
waf install
```

# Build Documentation
Run `waf build_docs` to build the Sphinx HTML documentation (outputs to `docs/build/html`).

```
waf build_docs
```

To clean the built documentation:

```
waf clean_docs
```

# Example
It is possible to create just the shared memory for an example. See examples below
## staring a simple clock
The first example is a simple C code increasing a counter in the shared memory.
This software clock can be used to synchronize different program using built-in semaphore in the DAO SHM
### create SHM
can be skipped if SHM already created
```
import daoShm
import numpy as np
clockShm=daoShm.shm('/tmp/demoClockShm.im.shm',np.zeros((1,1)).astype(np.uint32))
``` 
### run example
Now let's run the clock at 1.5kHz in a new console
```
daoClock -L demoClockShm 1500
```
## writing data in shared memory
This example is a C program writing in the shared memory at a specific rate.
The program is waiting for a new value in another shared memory. We can use the previous example and use the clock as the trigger
### create SHM
can be skipped if SHM already created
```
import daoShm
import numpy as np
shm=daoShm.shm('/tmp/demoShm.im.shm',np.zeros((100,100)).astype(np.float32))
clockShm=daoShm.shm('/tmp/demoClockShm.im.shm',np.zeros((1,1)).astype(np.uint32))
``` 
### run example
Now run in a new console the program writing at the clock rate in the share memory random values
```
daoRandWriterSync -L demoShm demoClockShm
```
