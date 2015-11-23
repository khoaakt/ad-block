#include "ABPFilterParser.h"
#include <string.h>
#include <stdio.h>

#ifdef PERF_STATS
#include <iostream>
using namespace std;
#endif

#include <iostream>
using namespace std;

#include "badFingerprints.h"

const int maxLineLength = 2048;

enum FilterParseState {
  FPStart,
  FPPastWhitespace,
  FPOneBar,
  FPOneAt,
  FPData,
  // Same as data but won't consider any special char handling like | or $
  FPDataOnly
};

static const int fingerprintSize = 6;

/**
 * Finds the host within the passed in URL and returns its length
 */
const char * getUrlHost(const char *input, int &len) {
  const char *p = input;
  while (*p != '\0' && *p != ':') {
    p++;
  }
  if (*p != '\0') {
    p++;
    while (*p != '\0' && *p == '/') {
      p++;
    }
  }
  const char *q = p;
  while (*q != '\0') {
    q++;
  }
  len = findFirstSeparatorChar(p, q);
  return p;
}


inline bool isFingerprintChar(char c) {
  return c != '|' && c != '*' && c != '^';
}

bool isBadFingerprint(const char *fingerprint, const char * fingerprintEnd) {
  for (unsigned int i = 0; i < sizeof(badFingerprints)/sizeof(badFingerprints[0]); i++) {
    if(!strncmp(badFingerprints[i], fingerprint, fingerprintEnd - fingerprint)) {
      return true;
    }
  }
  return false;
}

bool hasBadSubstring(const char *fingerprint, const char * fingerprintEnd) {
  for (unsigned int i = 0; i < sizeof(badSubstrings)/sizeof(badSubstrings[0]); i++) {
    const char * p = strstr(fingerprint, badSubstrings[i]);
    if (p && (p - fingerprint) + strlen(badSubstrings[i]) <= (unsigned int)(fingerprintEnd - fingerprint)) {
      return true;
    }
  }
  return false;
}

/**
 * Obtains a fingerprint for the specified filter
 */
bool getFingerprint(char *buffer, const char *input) {
  if (!input) {
    return false;
  }
  int size = 0;
  const char *p = input;
  const char *start = input;
  while (*p != '\0') {
    if (!isFingerprintChar(*p)) {
      size = 0;
      p++;
      start = p;
      continue;
    }
    if (buffer) {
      buffer[size] = *p;
    }
    if (hasBadSubstring(start, start + size + 1)) {
      size = 0;
      start++;
      p = start;
      continue;
    }
    size++;

    if (size == fingerprintSize) {
      if (buffer) {
        buffer[size] = '\0';
      }
      if (isBadFingerprint(start, start + size)) {
        size = 0;
        start++;
        p = start;
        continue;
      }
      return true;
    }
    p++;
  }
  if (buffer) {
    buffer[0] = '\0';
  }
  return false;
}

bool getFingerprint(char *buffer, Filter &f) {
  if (f.filterType & FTRegex) {
    // cout << "Get fingerprint for regex returning false; " << endl;
    return false;
  }

  if (f.filterType & FTHostAnchored) {
    if (getFingerprint(buffer, f.data + strlen(f.host))) {
      return true;
    }
  }

  bool b = getFingerprint(buffer, f.data);
  // if (!b && f.data) {
  //   cout << "No fingerprint for: " << f.data << endl;
  // }
  return b;
}

// Separator chars are one of: :?/=^;
char separatorBuffer[32] = { 0, 0, 0, 0, 16, -128, 0, -92, 0, 0, 0, 64 };
bool isSeparatorChar(char c) {
  return !!(separatorBuffer[(unsigned char)c / 8] & 1 << (unsigned char)c % 8);
}

int findFirstSeparatorChar(const char *input, const char *end) {
  const char *p = input;
  while (p != end) {
    if (isSeparatorChar(*p)) {
      return p - input;
    }
    p++;
  }
  return end - input;
}

void parseFilter(const char *input, Filter &f, BloomFilter *bloomFilter, BloomFilter *exceptionBloomFilter, HashSet<Filter> *hostAnchoredHashSet, HashSet<Filter> *hostAnchoredExceptionHashSet, HashSet<CosmeticFilter> *simpleCosmeticFilters) {
  const char *end = input;
  while (*end != '\0') end++;
  parseFilter(input, end, f, bloomFilter, exceptionBloomFilter, hostAnchoredHashSet, hostAnchoredExceptionHashSet, simpleCosmeticFilters);
}

// Not currently multithreaded safe due to the static buffer named 'data'
void parseFilter(const char *input, const char *end, Filter &f, BloomFilter *bloomFilter, BloomFilter *exceptionBloomFilter, HashSet<Filter> *hostAnchoredHashSet, HashSet<Filter> *hostAnchoredExceptionHashSet, HashSet<CosmeticFilter> *simpleCosmeticFilters) {
  FilterParseState parseState = FPStart;
  const char *p = input;
  char data[maxLineLength];
  memset(data, 0, sizeof data);
  int i = 0;

  bool earlyBreak = false;
  while (p != end && !earlyBreak) {
    // Check for the filter being too long
    if ((p - input) >= maxLineLength - 1) {
      return;
    }

    if (parseState != FPDataOnly) {
      if (parseState == FPOneBar && *p != '|') {
        parseState = FPData;
        f.filterType = static_cast<FilterType>(f.filterType | FTLeftAnchored);
      }

      switch (*p) {
        case '|':
          if (parseState == FPStart || parseState == FPPastWhitespace) {
            parseState = FPOneBar;
            p++;
            continue;
          } else if (parseState == FPOneBar) {
            parseState = FPOneBar;
            f.filterType = static_cast<FilterType>(f.filterType | FTHostAnchored);
            parseState = FPData;
            p++;

            int len = findFirstSeparatorChar(p, end);
            f.host = new char[len + 1];
            f.host[len] = '\0';
            memcpy(f.host, p, len);

            if ((*(p + len) == '^' && (*(p + len + 1) == '\0' || *(p + len + 1) == '$' || *(p + len + 1) == '\n')) ||
                *(p + len) == '\0' || *(p + len) == '$' || *(p + len) == '\n') {
              f.filterType = static_cast<FilterType>(f.filterType | FTHostOnly);
            }

            continue;
          } else {
            f.filterType = static_cast<FilterType>(f.filterType | FTRightAnchored);
            parseState = FPData;
            p++;
            continue;
          }
          break;
        case '@':
          if (parseState == FPStart || parseState == FPPastWhitespace) {
            parseState = FPOneAt;
            p++;
            continue;
          } else if (parseState == FPOneAt) {
            parseState = FPOneBar;
            f.filterType = FTException;
            parseState = FPPastWhitespace;
            p++;
            continue;
          }
          break;
        case '!':
        case '[':
          if (parseState == FPStart || parseState == FPPastWhitespace) {
            f.filterType = FTComment;
            // Wed don't care about comments right now
            return;
          }
          break;
        case '\r':
        case '\n':
        case '\t':
        case ' ':
          // Skip leading whitespace
          if (parseState == FPStart) {
            p++;
            continue;
          }
          break;
        case '/':
          if ((parseState == FPStart || parseState == FPPastWhitespace) && input[strlen(input) -1] == '/') {
            // Just copy out the whole regex and return early
            int len = strlen(input) - i - 1;
            f.data = new char[len];
            f.data[len - 1] = '\0';
            memcpy(f.data, input + i + 1, len - 1);
            f.filterType = FTRegex;
            return;
          }
          break;

        case '$':
          f.parseOptions(p + 1);
          earlyBreak = true;
          continue;
        case '#':
          if (*(p+1) == '#' || *(p+1) == '@') {
            if (i != 0) {
              f.domainList = new char [i + 1];
              memcpy(f.domainList, data, i + 1);
              i = 0;
            }
            parseState = FPDataOnly;
            if (*(p+1) == '#') {
              f.filterType = FTElementHiding;
            } else {
              f.filterType = FTElementHidingException;
            }
            p += 2;
            continue;
          }
        default:
          parseState = FPData;
          break;
      }
    }
    data[i] = *p;
    i++;
    p++;
  }

  if (parseState == FPStart) {
    f.filterType = FTEmpty;
    return;
  }

  data[i] = '\0';
  f.data = new char[i + 1];
  memcpy(f.data, data, i + 1);

  char fingerprintBuffer[fingerprintSize + 1];
  fingerprintBuffer[fingerprintSize] = '\0';

  if (f.filterType == FTElementHiding) {
    if (simpleCosmeticFilters && !f.domainList) {
      simpleCosmeticFilters->add(data);
    }
  } else if (f.filterType == FTElementHidingException) {
    if (simpleCosmeticFilters && f.domainList) {
      simpleCosmeticFilters->remove(data);
    }
  } else if (exceptionBloomFilter && (f.filterType & FTException) && (f.filterType & FTHostOnly)) {
    // cout << "add host anchored exception bloom filter: " << f.host << endl;
    hostAnchoredExceptionHashSet->add(f);
  } else if (hostAnchoredHashSet && (f.filterType & FTHostOnly)) {
    // cout << "add host anchored bloom filter: " << f.host << endl;
    hostAnchoredHashSet->add(f);
  } else if (getFingerprint(fingerprintBuffer, f)) {
    if (exceptionBloomFilter && f.filterType & FTException) {
      exceptionBloomFilter->add(fingerprintBuffer);
    } else if (bloomFilter) {
      // cout << "add fingerprint: " << fingerprintBuffer << ", from string: " << f.data << endl;
      bloomFilter->add(fingerprintBuffer);
    }
  }
}


ABPFilterParser::ABPFilterParser() : filters(nullptr),
  htmlRuleFilters(nullptr),
  exceptionFilters(nullptr),
  noFingerprintFilters(nullptr),
  noFingerprintExceptionFilters(nullptr),
  numFilters(0),
  numHtmlRuleFilters(0),
  numExceptionFilters(0),
  numNoFingerprintFilters(0),
  numNoFingerprintExceptionFilters(0),
  numHostAnchoredFilters(0),
  numHostAnchoredExceptionFilters(0),
  bloomFilter(nullptr),
  exceptionBloomFilter(nullptr),
  hostAnchoredHashSet(nullptr),
  hostAnchoredExceptionHashSet(nullptr),
  badFingerprintsHashSet(nullptr),
  numFalsePositives(0),
  numExceptionFalsePositives(0),
  numBloomFilterSaves(0),
  numExceptionBloomFilterSaves(0) {
}

ABPFilterParser::~ABPFilterParser() {
  if (filters) {
    delete[] filters;
  }
  if( htmlRuleFilters) {
    delete[] htmlRuleFilters;
  }
  if (exceptionFilters) {
    delete[] exceptionFilters;
  }
  if (noFingerprintFilters) {
   delete[] noFingerprintFilters;
  }
  if (noFingerprintExceptionFilters) {
    delete[] noFingerprintExceptionFilters;
  }
  if (bloomFilter) {
    delete bloomFilter;
  }
  if (exceptionBloomFilter) {
    delete exceptionBloomFilter;
  }
  if (hostAnchoredHashSet) {
    delete hostAnchoredHashSet;
  }
  if (hostAnchoredExceptionHashSet) {
    delete hostAnchoredExceptionHashSet;
  }
  if (badFingerprintsHashSet) {
    delete badFingerprintsHashSet;
  }
}

bool ABPFilterParser::hasMatchingFilters(Filter *filter, int &numFilters, const char *input, int inputLen, FilterOption contextOption, const char *contextDomain, BloomFilter *inputBloomFilter, const char *inputHost, int inputHostLen) {
  for (int i = 0; i < numFilters; i++) {
    if (filter->matches(input, inputLen, contextOption, contextDomain, inputBloomFilter, inputHost, inputHostLen)) {
      return true;
    }
    filter++;
  }
  return false;
}

void discoverMatchingPrefix(BadFingerprintsHashSet *badFingerprintsHashSet, const char *str, BloomFilter *bloomFilter, int prefixLen = fingerprintSize) {
  char sz[32];
  memset(sz, 0, sizeof(sz));
  int strLen = strlen(str);
  for (int i = 0; i < strLen - prefixLen + 1; i++) {
    if (bloomFilter->exists(str + i, prefixLen)) {
      memcpy(sz, str + i, prefixLen);
      // cout <<  "Bad fingerprint: " << sz << endl;
      if (badFingerprintsHashSet) {
        badFingerprintsHashSet->add(BadFingerprint(sz));
      }
    } else {
      // memcpy(sz, str + i, prefixLen);
      // cout <<  "Good fingerprint: " << sz;
    }
  }
}

bool isHostAnchoredHashSetMiss(const char *input, int inputLen, HashSet<Filter> *hashSet, const char *inputHost, int inputHostLen, FilterOption contextOption, const char *contextDomain) {
  if (!hashSet) {
    return false;
  }

  const char *start = inputHost + inputHostLen;
  // Skip past the TLD
  while (start != inputHost) {
    start--;
    if (*(start) == '.') {
      break;
    }
  }

  while (start != inputHost) {
    if (*(start - 1) == '.') {
      Filter *filter = hashSet->find(Filter(start, inputHost + inputHostLen - start, nullptr, start));
      if (filter && filter->matches(input, inputLen, contextOption, contextDomain)) {
        return false;
      }
    }
    start--;
  }

  Filter *filter = hashSet->find(Filter(start, inputHost + inputHostLen - start, nullptr, start));
  if (!filter) {
    return true;
  }
  return !filter->matches(input, inputLen, contextOption, contextDomain);
}

bool ABPFilterParser::matches(const char *input, FilterOption contextOption, const char *contextDomain) {
  int inputLen = strlen(input);
  int inputHostLen;
  const char *inputHost = getUrlHost(input, inputHostLen);

  // Optmization for the manual filter checks which are needed.
  // Avoid having to check individual filters if the filter parts are not found
  // inside the input bloom filter.
  HashFn2Byte hashFns[] = { HashFn2Byte() };
  BloomFilter inputBloomFilter(10, 1024, hashFns, 1);
  for (int i = 1; i < inputLen; i++) {
    inputBloomFilter.add(input + i - 1, 2);
  }

  // We always have to check noFingerprintFilters because the bloom filter opt cannot be used for them
  bool hasMatch = hasMatchingFilters(noFingerprintFilters, numNoFingerprintFilters, input, inputLen, contextOption, contextDomain, &inputBloomFilter, inputHost, inputHostLen);
  // If no noFingerprintFilters were hit, check the bloom filter substring fingerprint for the normal
  // filter list.   If no substring exists for the input then we know for sure the URL should not be blocked.
  bool bloomFilterMiss = false;
  bool hostAnchoredHashSetMiss = false;
  if (!hasMatch) {
    bloomFilterMiss = bloomFilter && !bloomFilter->substringExists(input, fingerprintSize);
    hostAnchoredHashSetMiss = isHostAnchoredHashSetMiss(input, inputLen, hostAnchoredHashSet, inputHost, inputHostLen, contextOption, contextDomain);
    if (bloomFilterMiss && hostAnchoredHashSetMiss) {
      numBloomFilterSaves++;
      return false;
    }

    hasMatch = !hostAnchoredHashSetMiss;
  }

  // We need to check the filters list manually because there is either a match or a false positive
  if (!hasMatch && !bloomFilterMiss) {
    hasMatch = hasMatchingFilters(filters, numFilters, input, inputLen, contextOption, contextDomain, &inputBloomFilter, inputHost, inputHostLen);
    // If there's still no match after checking the block filters, then no need to try to block this
    // because there is a false positive.
    if (!hasMatch) {
      numFalsePositives++;
      if (badFingerprintsHashSet) {
        // cout << "false positive for input: " << input << " bloomFilterMiss: " << bloomFilterMiss << ", hostAnchoredHashSetMiss: " << hostAnchoredHashSetMiss << endl;
        discoverMatchingPrefix(badFingerprintsHashSet, input, bloomFilter);
      }
      return false;
    }
  }

  // If there's a matching no fingerprint exception then we can just return right away because we shouldn't block
  if (hasMatchingFilters(noFingerprintExceptionFilters, numNoFingerprintExceptionFilters, input, inputLen, contextOption, contextDomain, &inputBloomFilter, inputHost, inputHostLen)) {
    return false;
  }

  bool bloomExceptionFilterMiss = exceptionBloomFilter && !exceptionBloomFilter->substringExists(input, fingerprintSize);
  bool hostAnchoredExceptionHashSetMiss = isHostAnchoredHashSetMiss(input, inputLen, hostAnchoredExceptionHashSet, inputHost, inputHostLen, contextOption, contextDomain);

  // Now that we have a matching rule, we should check if no exception rule hits, if none hits, we should block
  if (bloomExceptionFilterMiss && hostAnchoredExceptionHashSetMiss) {
    numExceptionBloomFilterSaves++;
    return true;
  }

  if (!bloomExceptionFilterMiss) {
    if (!hasMatchingFilters(exceptionFilters, numExceptionFilters, input, inputLen, contextOption, contextDomain, &inputBloomFilter, inputHost, inputHostLen)) {
      // False positive on the exception filter list
      numExceptionFalsePositives++;
      // cout << "exception false positive for input: " << input << endl;
      if (badFingerprintsHashSet) {
        discoverMatchingPrefix(badFingerprintsHashSet, input, exceptionBloomFilter);
      }
      return true;
    }
  }

  return false;
}

void ABPFilterParser::initBloomFilter(BloomFilter **pp, const char *buffer, int len) {
  if (*pp) {
    delete *pp;
  }
  if (len > 0) {
    *pp = new BloomFilter(buffer, len);
  }
}

void ABPFilterParser::initHashSet(HashSet<Filter> **pp, char *buffer, int len) {
  if (*pp) {
    delete *pp;
  }
  if (len > 0) {
    *pp = new HashSet<Filter>(0);
    (*pp)->deserialize(buffer, len);
  }
}

void setFilterBorrowedMemory(Filter *filters, int numFilters) {
  for (int i = 0; i < numFilters; i++) {
    filters[i].borrowedData = true;
  }
}

// Parses the filter data into a few collections of filters and enables efficent querying
bool ABPFilterParser::parse(const char *input) {
  // If the user is parsing and we have regex support,
  // then we can determine the fingerprints for the bloom filter.
  // Otherwise it needs to be done manually via initBloomFilter and initExceptionBloomFilter
  if (!bloomFilter) {
    bloomFilter = new BloomFilter(10, 40000);
  }
  if (!exceptionBloomFilter) {
    exceptionBloomFilter = new BloomFilter(10, 10000);
  }
  if (!hostAnchoredHashSet) {
    // Optimized to be 1:1 with the easylist number of host anchored hosts
    hostAnchoredHashSet = new HashSet<Filter>(5395);
  }
  if (!hostAnchoredExceptionHashSet) {
    // Optimized to be 1:1 with the easylist number of host anchored exception hosts
    hostAnchoredExceptionHashSet = new HashSet<Filter>(428);
  }

  const char *p = input;
  const char *lineStart = p;

  int newNumFilters = 0;
  int newNumHtmlRuleFilters = 0;
  int newNumExceptionFilters = 0;
  int newNumNoFingerprintFilters = 0;
  int newNumNoFingerprintExceptionFilters = 0;
  int newNumHostAnchoredFilters = 0;
  int newNumHostAnchoredExceptionFilters = 0;

  // Simple cosmetic filters apply to all sites without exception
  HashSet<CosmeticFilter> simpleCosmeticFilters(1000);

  // Parsing does 2 passes, one just to determine the type of information we'll need to setup.
  // Note that the library will be used on a variety of builds so sometimes we won't even have STL
  // So we can't use something like a vector here.
  while (true) {
    if (*p == '\n' || *p == '\0') {
      Filter f;
      parseFilter(lineStart, p, f);
      switch(f.filterType & FTListTypesMask) {
        case FTException:
          if (f.filterType & FTHostOnly) {
            newNumHostAnchoredExceptionFilters++;
          } else if (getFingerprint(nullptr, f)) {
            newNumExceptionFilters++;
          } else {
            newNumNoFingerprintExceptionFilters++;
          }
          break;
        case FTElementHiding:
          newNumHtmlRuleFilters++;
          break;
        case FTElementHidingException:
          newNumHtmlRuleFilters++;
          break;
        case FTEmpty:
        case FTComment:
          // No need to store comments
          break;
        default:
          if (f.filterType & FTHostOnly) {
            newNumHostAnchoredFilters++;
          } else if (getFingerprint(nullptr, f)) {
            newNumFilters++;
          } else {
            newNumNoFingerprintFilters++;
          }
          break;
      }
      lineStart = p + 1;
    }

    if (*p == '\0') {
      break;
    }

    p++;
  };


#ifdef PERF_STATS
  cout << "Fingerprint size: " << fingerprintSize << endl;
  cout << "Num new filters: " << newNumFilters << endl;
  cout << "Num new HTML rule filters: " << newNumHtmlRuleFilters << endl;
  cout << "Num new exception filters: " << newNumExceptionFilters << endl;
  cout << "Num new no fingerprint filters: " << newNumNoFingerprintFilters << endl;
  cout << "Num new no fingerprint exception filters: " << newNumNoFingerprintExceptionFilters << endl;
  cout << "Num new host anchored filters: " << newNumHostAnchoredFilters << endl;
  cout << "Num new host anchored exception filters: " << newNumHostAnchoredExceptionFilters << endl;
#endif

  Filter *newFilters = new Filter[newNumFilters + numFilters];
  Filter *newHtmlRuleFilters = new Filter[newNumHtmlRuleFilters + numHtmlRuleFilters];
  Filter *newExceptionFilters = new Filter[newNumExceptionFilters + numExceptionFilters];
  Filter *newNoFingerprintFilters = new Filter[newNumNoFingerprintFilters + numNoFingerprintFilters];
  Filter *newNoFingerprintExceptionFilters = new Filter[newNumNoFingerprintExceptionFilters + numNoFingerprintExceptionFilters];

  memset(newFilters, 0, sizeof(Filter) * (newNumFilters + numFilters));
  memset(newHtmlRuleFilters, 0, sizeof(Filter) * (newNumHtmlRuleFilters + numHtmlRuleFilters));
  memset(newExceptionFilters, 0, sizeof(Filter) * (newNumExceptionFilters + numExceptionFilters));
  memset(newNoFingerprintFilters, 0, sizeof(Filter) * (newNumNoFingerprintFilters + numNoFingerprintFilters));
  memset(newNoFingerprintExceptionFilters, 0, sizeof(Filter) * (newNumNoFingerprintExceptionFilters + numNoFingerprintExceptionFilters));

  Filter *curFilters = newFilters;
  Filter *curHtmlRuleFilters = newHtmlRuleFilters;
  Filter *curExceptionFilters = newExceptionFilters;
  Filter *curNoFingerprintFilters = newNoFingerprintFilters;
  Filter *curNoFingerprintExceptionFilters = newNoFingerprintExceptionFilters;

  // If we've had a parse before copy the old data into the new data structure
  if (filters || htmlRuleFilters || exceptionFilters || noFingerprintFilters || noFingerprintExceptionFilters /*|| hostAnchoredFilters || hostAnchoredExceptionFilters */) {
    // Copy the old data in
    memcpy(newFilters, filters, sizeof(Filter) * numFilters);
    memcpy(newHtmlRuleFilters, htmlRuleFilters, sizeof(Filter) * numHtmlRuleFilters);
    memcpy(newExceptionFilters, exceptionFilters, sizeof(Filter) * numExceptionFilters);
    memcpy(newNoFingerprintFilters, noFingerprintFilters, sizeof(Filter) * (numNoFingerprintFilters));
    memcpy(newNoFingerprintExceptionFilters, noFingerprintExceptionFilters, sizeof(Filter) * (numNoFingerprintExceptionFilters));

    // Free up the old memory for filter storage
    // Set the old filter lists borrwedMemory to true since it'll be taken by the new filters.
    setFilterBorrowedMemory(filters, numFilters);
    setFilterBorrowedMemory(htmlRuleFilters, numHtmlRuleFilters);
    setFilterBorrowedMemory(exceptionFilters, numExceptionFilters);
    setFilterBorrowedMemory(noFingerprintFilters, numNoFingerprintFilters);
    setFilterBorrowedMemory(noFingerprintExceptionFilters, numNoFingerprintExceptionFilters);
    delete[] filters;
    delete[] htmlRuleFilters;
    delete[] exceptionFilters;
    delete[] noFingerprintFilters;
    delete[] noFingerprintExceptionFilters;

    // Adjust the current pointers to be just after the copied in data
    curFilters += numFilters;
    curHtmlRuleFilters += numHtmlRuleFilters;
    curExceptionFilters += numExceptionFilters;
    curNoFingerprintFilters += numNoFingerprintFilters;
    curNoFingerprintExceptionFilters += numNoFingerprintExceptionFilters;
  }

  // And finally update with the new counts
  numFilters += newNumFilters;
  numHtmlRuleFilters += newNumHtmlRuleFilters;
  numExceptionFilters += newNumExceptionFilters;
  numNoFingerprintFilters += newNumNoFingerprintFilters;
  numNoFingerprintExceptionFilters += newNumNoFingerprintExceptionFilters;
  numHostAnchoredFilters += newNumHostAnchoredFilters;
  numHostAnchoredExceptionFilters += newNumHostAnchoredExceptionFilters;

  // Adjust the new member list pointers
  filters = newFilters;
  htmlRuleFilters = newHtmlRuleFilters;
  exceptionFilters = newExceptionFilters;
  noFingerprintFilters = newNoFingerprintFilters;
  noFingerprintExceptionFilters = newNoFingerprintExceptionFilters;

  p = input;
  lineStart = p;

  while (true) {
    if (*p == '\n' || *p == '\0') {
      Filter f;
      parseFilter(lineStart, p, f, bloomFilter, exceptionBloomFilter, hostAnchoredHashSet, hostAnchoredExceptionHashSet, &simpleCosmeticFilters);
      switch(f.filterType & FTListTypesMask) {
        case FTException:
          if (f.filterType & FTHostOnly) {
            // do nothing, handled by hash set.
          } else if (getFingerprint(nullptr, f)) {
            (*curExceptionFilters).swap(f);
            curExceptionFilters++;
          } else {
            (*curNoFingerprintExceptionFilters).swap(f);
            curNoFingerprintExceptionFilters++;
          }
          break;
        case FTElementHiding:
        case FTElementHidingException:
          (*curHtmlRuleFilters).swap(f);
          curHtmlRuleFilters++;
          break;
        case FTEmpty:
        case FTComment:
          // No need to store
          break;
        default:
          if (f.filterType & FTHostOnly) {
            // Do nothing
          } else if (getFingerprint(nullptr, f)) {
            (*curFilters).swap(f);
            curFilters++;
          } else {
            (*curNoFingerprintFilters).swap(f);
            curNoFingerprintFilters++;
          }
          break;
      }
      lineStart = p + 1;
    }

    if (*p == '\0') {
      break;
    }

    p++;
  };

  cout << "Simple cosmetic filter size: " << simpleCosmeticFilters.size() << endl;
  return true;
}

// Fills the specified buffer if specified, returns the number of characters written or needed
int serializeFilters(char * buffer, Filter *f, int numFilters) {
  char sz[256];
  int bufferSize = 0;
  for (int i = 0; i < numFilters; i++) {
    int sprintfLen = sprintf(sz, "%x,%x,%x", (int)f->filterType, (int)f->filterOption, (int)f->antiFilterOption);
    if (buffer) {
      strcpy(buffer + bufferSize, sz);
    }
    bufferSize += sprintfLen;
    // Extra null termination
    bufferSize++;

    if (f->data) {
      if (buffer) {
        strcpy(buffer + bufferSize, f->data);
      }
      bufferSize += strlen(f->data);
    }
    bufferSize++;

    if (f->domainList) {
      if (buffer) {
        strcpy(buffer + bufferSize, f->domainList);
      }
      bufferSize += strlen(f->domainList);
    }
    // Extra null termination
    bufferSize++;
    if (f->host) {
      if (buffer) {
        strcpy(buffer + bufferSize, f->host);
      }
      bufferSize += strlen(f->host);
    }
    // Extra null termination
    bufferSize++;
    f++;
  }
  return bufferSize;
}

// Returns a newly allocated buffer, caller must manually delete[] the buffer
char * ABPFilterParser::serialize(int &totalSize, bool ignoreHTMLFilters) {
  totalSize = 0;
  int adjustedNumHTMLFilters = ignoreHTMLFilters ? 0 : numHtmlRuleFilters;

  uint32_t hostAnchoredHashSetSize = 0;
  char *hostAnchoredHashSetBuffer = nullptr;
  if (hostAnchoredHashSet) {
    hostAnchoredHashSetBuffer = hostAnchoredHashSet->serialize(hostAnchoredHashSetSize);
  }

  uint32_t hostAnchoredExceptionHashSetSize = 0;
  char *hostAnchoredExceptionHashSetBuffer = nullptr;
  if (hostAnchoredExceptionHashSet) {
    hostAnchoredExceptionHashSetBuffer = hostAnchoredExceptionHashSet->serialize(hostAnchoredExceptionHashSetSize);
  }

  // Get the number of bytes that we'll need
  char sz[512];
  totalSize += 1 + sprintf(sz,  "%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x", numFilters, numExceptionFilters, adjustedNumHTMLFilters, numNoFingerprintFilters, numNoFingerprintExceptionFilters, numHostAnchoredFilters, numHostAnchoredExceptionFilters,
      bloomFilter ? bloomFilter->getByteBufferSize() : 0, exceptionBloomFilter ? exceptionBloomFilter->getByteBufferSize() : 0, hostAnchoredHashSetSize, hostAnchoredExceptionHashSetSize);
  totalSize += serializeFilters(nullptr, filters, numFilters) +
    serializeFilters(nullptr, exceptionFilters, numExceptionFilters) +
    serializeFilters(nullptr, htmlRuleFilters, adjustedNumHTMLFilters) +
    serializeFilters(nullptr, noFingerprintFilters, numNoFingerprintFilters) +
    serializeFilters(nullptr, noFingerprintExceptionFilters, numNoFingerprintExceptionFilters);
  totalSize += bloomFilter ? bloomFilter->getByteBufferSize() : 0;
  totalSize += exceptionBloomFilter ? exceptionBloomFilter->getByteBufferSize() : 0;
  totalSize += hostAnchoredHashSetSize;
  totalSize += hostAnchoredExceptionHashSetSize;

  // Allocate it
  int pos = 0;
  char *buffer = new char[totalSize];
  memset(buffer, 0, totalSize);

  // And start copying stuff in
  strcpy(buffer, sz);
  pos += strlen(sz) + 1;
  pos += serializeFilters(buffer + pos, filters, numFilters);
  pos += serializeFilters(buffer + pos, exceptionFilters, numExceptionFilters);
  pos += serializeFilters(buffer + pos, htmlRuleFilters, adjustedNumHTMLFilters);
  pos += serializeFilters(buffer + pos, noFingerprintFilters, numNoFingerprintFilters);
  pos += serializeFilters(buffer + pos, noFingerprintExceptionFilters, numNoFingerprintExceptionFilters);
  if (bloomFilter) {
    memcpy(buffer + pos, bloomFilter->getBuffer(), bloomFilter->getByteBufferSize());
    pos += bloomFilter->getByteBufferSize();
  }
  if (exceptionBloomFilter) {
    memcpy(buffer + pos, exceptionBloomFilter->getBuffer(), exceptionBloomFilter->getByteBufferSize());
    pos += exceptionBloomFilter->getByteBufferSize();
  }
  if (hostAnchoredHashSet) {
    memcpy(buffer + pos, hostAnchoredHashSetBuffer, hostAnchoredHashSetSize);
    pos += hostAnchoredHashSetSize;
    delete hostAnchoredHashSetBuffer;
  }
  if (hostAnchoredExceptionHashSet) {
    memcpy(buffer + pos, hostAnchoredExceptionHashSetBuffer, hostAnchoredExceptionHashSetSize);
    pos += hostAnchoredExceptionHashSetSize;
    delete hostAnchoredExceptionHashSetBuffer;
  }
  return buffer;
}

// Fills the specified buffer if specified, returns the number of characters written or needed
int deserializeFilters(char *buffer, Filter *f, int numFilters) {
  int pos = 0;
  for (int i = 0; i < numFilters; i++) {
    f->borrowedData = true;
    sscanf(buffer + pos, "%x,%x,%x", &f->filterType, &f->filterOption, &f->antiFilterOption);
    pos += strlen(buffer + pos) + 1;

    if (*(buffer + pos) == '\0') {
      f->data = nullptr;
    } else {
      f->data = buffer + pos;
      pos += strlen(f->data);
    }
    pos++;

   if (*(buffer + pos) == '\0') {
      f->domainList = nullptr;
    } else {
      f->domainList = buffer + pos;
      pos += strlen(f->domainList);
    }
    pos++;

   if (*(buffer + pos) == '\0') {
      f->host = nullptr;
    } else {
      f->host = buffer + pos;
      pos += strlen(f->host);
    }
    pos++;
    f++;
  }
  return pos;
}

void ABPFilterParser::deserialize(char *buffer) {
  int bloomFilterSize = 0, exceptionBloomFilterSize = 0, hostAnchoredHashSetSize = 0, hostAnchoredExceptionHashSetSize = 0;
  int pos = 0;
  sscanf(buffer + pos, "%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x", &numFilters, &numExceptionFilters, &numHtmlRuleFilters, &numNoFingerprintFilters, &numNoFingerprintExceptionFilters, &numHostAnchoredFilters, &numHostAnchoredExceptionFilters, &bloomFilterSize, &exceptionBloomFilterSize, &hostAnchoredHashSetSize, &hostAnchoredExceptionHashSetSize);
  pos += strlen(buffer + pos) + 1;

  filters = new Filter[numFilters];
  exceptionFilters = new Filter[numExceptionFilters];
  htmlRuleFilters = new Filter[numHtmlRuleFilters];
  noFingerprintFilters = new Filter[numNoFingerprintFilters];
  noFingerprintExceptionFilters = new Filter[numNoFingerprintExceptionFilters];

  pos += deserializeFilters(buffer + pos, filters, numFilters);
  pos += deserializeFilters(buffer + pos, exceptionFilters, numExceptionFilters);
  pos += deserializeFilters(buffer + pos, htmlRuleFilters, numHtmlRuleFilters);
  pos += deserializeFilters(buffer + pos, noFingerprintFilters, numNoFingerprintFilters);
  pos += deserializeFilters(buffer + pos, noFingerprintExceptionFilters, numNoFingerprintExceptionFilters);

  initBloomFilter(&bloomFilter, buffer + pos, bloomFilterSize);
  pos += bloomFilterSize;
  initBloomFilter(&exceptionBloomFilter, buffer + pos, exceptionBloomFilterSize);
  pos += exceptionBloomFilterSize;
  initHashSet(&hostAnchoredHashSet, buffer + pos, hostAnchoredHashSetSize);
  pos += hostAnchoredHashSetSize;
  initHashSet(&hostAnchoredExceptionHashSet, buffer + pos, hostAnchoredExceptionHashSetSize);
  pos += hostAnchoredExceptionHashSetSize;
}

void ABPFilterParser::enableBadFingerprintDetection() {
  if (badFingerprintsHashSet) {
    return;
  }

  badFingerprintsHashSet = new BadFingerprintsHashSet();
  for (unsigned int i = 0; i < sizeof(badFingerprints)/sizeof(badFingerprints[0]); i++) {
    badFingerprintsHashSet->add(BadFingerprint(badFingerprints[i]));
  }
}
