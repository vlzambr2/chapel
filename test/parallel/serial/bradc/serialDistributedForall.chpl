use BlockDist;

config const n = 1000;

var D = {1..n} dmapped blockDist({1..n});

serial {
  forall i in D do
    writeln(i);
}
