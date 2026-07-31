# Real project: text analytics — manual word splitting, frequency dict,
# deterministic top-word selection, aggregate stats.
text = "the quick brown fox jumps over the lazy dog the fox is quick and the dog is lazy"

words = []
cur = ""
i = 0
while i < len(text):
    c = text[i]
    if c == " ":
        if len(cur) > 0:
            words.append(cur)
        cur = ""
    else:
        cur += c
    i += 1
if len(cur) > 0:
    words.append(cur)

freq = {}
for w in words:
    freq[w] = freq.get(w, 0) + 1

best = ""
bestn = 0
for w in freq.keys():
    n = freq[w]
    if n > bestn or (n == bestn and w < best):
        best = w
        bestn = n

total_len = 0
longest = ""
for w in words:
    total_len += len(w)
    if len(w) > len(longest):
        longest = w

print(len(words))
print(len(freq))
print(best, bestn)
print(total_len)
print(longest)
