# `liftoff`

SpaceX telemetry parser and modelling application written
in C++.

This takes the telemetry feed downloaded from SpaceX's
livestream and attempts to model the dynamic forces acting
on the rocket. The current iteration uses data from the
JCSAT-18/KACIFIC1 mission. The results are plotted using
MathGL's FLTK library as the model is updated. This then
uses the processed data in a new model that attempts to
replicate the rocket to validate the model and extract
other data such as thrust and other controls.

# Demo

![Actual](https://i.postimg.cc/sx5NzD13/Screen-Shot-2020-12-20-at-8-53-25-PM.png)

# Build

Requires MathGL headers and GMP to be installed on your
computer.

``` shell
git clone https://github.com/AgentTroll/liftoff.git
cd liftoff
mkdir build && cd build
cmake .. && make

cd ..
./build/liftoff-cli/liftoff-cli
```

# Credits

Built with [CLion](https://www.jetbrains.com/clion/)

Utilizes:

  * [MathGL](http://mathgl.sourceforge.net/)
  * [SpaceXtract](https://github.com/shahar603/SpaceXtract)
  * [GMP](https://gmplib.org/)

A LOT of information was found on various different sites,
too many to list here. They've been pasted in various
different code files as comments.
