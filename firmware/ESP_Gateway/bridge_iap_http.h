#ifndef BRIDGE_IAP_HTTP_H
#define BRIDGE_IAP_HTTP_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* The HTTP body is firmware, never text. Stage one complete server block
 * before programming it, so a short TCP response can be retried safely. */
class IapHttpBlock {
  char header[1024];
  size_t headUsed=0, bodyUsed=0, expected=0;
  uint8_t *output=nullptr;
  uint32_t progressAt=0;
  bool headReady=false;
  const char *error=nullptr;
  unsigned code=0;
  static bool same(const char *a, const char *b) {
    while (*a && *b) {
      if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return false;
    }
    return *a==*b;
  }
  bool parseHeader() {
    unsigned major=0, minor=0;
    if (sscanf(header,"HTTP/%u.%u %u",&major,&minor,&code)!=3 || major!=1) {
      error="invalid HTTP status line"; return false;
    }
    if (code!=200) { error="HTTP status is not 200"; return false; }
    bool haveLength=false;
    char *line=strstr(header,"\r\n");
    if (!line) { error="invalid HTTP header"; return false; }
    line+=2;
    while (line[0] && line[0]!='\r') {
      char *end=strstr(line,"\r\n");
      if (!end) { error="invalid HTTP header line"; return false; }
      *end=0;
      char *colon=strchr(line,':');
      if (colon) {
        *colon=0;
        char *value=colon+1;
        while (*value==' ' || *value=='\t') ++value;
        if (same(line,"Content-Length")) {
          char *tail=nullptr;
          unsigned long length=strtoul(value,&tail,10);
          while (*tail==' ' || *tail=='\t') ++tail;
          if (haveLength || !isdigit((unsigned char)*value) || *tail || length!=expected) {
            error="Content-Length differs from expected firmware block"; return false;
          }
          haveLength=true;
        } else if (same(line,"Transfer-Encoding") && !same(value,"identity")) {
          error="encoded response is not a raw firmware block"; return false;
        }
        *colon=':';
      }
      *end='\r';
      line=end+2;
    }
    if (!haveLength) { error="missing Content-Length"; return false; }
    return true;
  }
public:
  void reset(uint8_t *target, size_t bytes, uint32_t now) {
    output=target; expected=bytes; headUsed=bodyUsed=0; progressAt=now;
    headReady=false; error=nullptr; code=0; header[0]=0;
    if (!target || !bytes || bytes>4096) error="invalid firmware block size";
  }
  bool feed(const uint8_t *data, size_t bytes, uint32_t now) {
    if (error) return false;
    if (bytes) progressAt=now;
    for (size_t i=0;i<bytes;i++) {
      if (!headReady) {
        if (!data[i] || headUsed+1>=sizeof(header)) {
          error="HTTP header too large or contains NUL"; return false;
        }
        header[headUsed++]=char(data[i]); header[headUsed]=0;
        if (headUsed>=4 && !memcmp(header+headUsed-4,"\r\n\r\n",4)) {
          if (!parseHeader()) return false;
          headReady=true;
        }
      } else {
        if (bodyUsed>=expected) { error="extra firmware body bytes"; return false; }
        output[bodyUsed++]=data[i];
      }
    }
    return true;
  }
  bool complete() const { return headReady && !error && bodyUsed==expected; }
  bool idleExpired(uint32_t now, uint32_t limit) const { return uint32_t(now-progressAt)>=limit; }
  size_t received() const { return bodyUsed; }
  const char *reason() const { return error ? error : "incomplete HTTP response"; }
  unsigned statusCode() const { return code; }
};
#endif
