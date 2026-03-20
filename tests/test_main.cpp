#include "../include/nrx/ref/nref.hpp"
int main() {
	nrx::ref::nref<int> i;
	i = 3;

	int x = 10;
	i = &x;

	nrx::ref::nref<int> i2 = &x;

}