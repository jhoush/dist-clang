// RUN: %clang_cc1 -analyze -analyzer-experimental-internal-checks -std=gnu99 -analyzer-check-objc-mem -analyzer-store=region -verify %s


// The store for 'a[1]' should not be removed mistakenly. SymbolicRegions may
// also be live roots.
void f14(int *a) {
  int i;
  a[1] = 1;
  i = a[1];
  if (i != 1) {
    int *p = 0;
    i = *p; // no-warning
  }
}
