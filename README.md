# cardputer-zero-rf

ZeroOS RF: a radio-focused edition of ZeroOS for the M5Stack Cardputer Zero Lite, built around a
USB software-defined radio dongle (RTL-SDR Blog V4 first, others later).

- [docs/PLAN.md](docs/PLAN.md): the architecture, the power, CPU and memory budget, the app list
  and the phases.
- [rf/](rf/README.md): the radio core (receiver backends, spectrum, the radio daemon and its
  protocol). Runs today against a simulated receiver; not yet tried on a real dongle.
