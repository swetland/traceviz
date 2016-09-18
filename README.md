# traceviz
System Trace Visualizer

## Setup And Build

Right now Linux is supported and libsdl2 and OpenGL are required.

On ubuntu you should be able to setup the pre-reqs with:
```
apt-get install libsdl2-dev
```

And check out and build like this:
```
git clone https://github.com/swetland/traceviz.git
cd traceviz
git submodule init
git submodule update
make
```

## Explore a Sample Trace File

Grab it from the samples branch:
```
git checkout origin/samples -- boot.trace
git rm --cached boot.trace
```

Run TraceViz:
```
./out/traceviz boot.trace
```

