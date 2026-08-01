# Bundled pure-Python stdlib: itertools + functools, compiled natively.
import itertools
from functools import reduce

print(itertools.permutations([1, 2, 3], 2))
print(itertools.combinations([1, 2, 3, 4], 2))
print(itertools.product([1, 2], [3, 4]))
print(itertools.accumulate([1, 2, 3, 4, 5]))
print(itertools.pairwise([10, 20, 30]))
print(reduce(lambda a, b: a + b, range(1, 6)))
print(reduce(lambda a, b: a * b, [1, 2, 3, 4], 2))
