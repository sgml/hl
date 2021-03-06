#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "hl.h"
#include "test_data.h"

void expect_eq(const char* expected, hl_token token) {
  int len = token.end - token.start;
  int expected_len = strlen(expected);

  if (len != expected_len) {
    printf("bad strlen. expected = %d, got = %d\n", expected_len, len);
    abort();
  }

  if (strncmp(token.start, expected, len) != 0) {
    printf("bad str. expected = \"%s\"\n", expected);
    abort();
  }
}


void test_req(const struct message* req) {
  hl_lexer lexer;
  hl_token token;
  const char* buf = req->raw;
  int raw_len = strlen(req->raw);
  int len = raw_len;
  int num_headers = 0;
  int body_len = strlen(req->body);
  char* body = malloc(body_len);
  int body_read = 0;
  int chunk_len;

  hl_req_init(&lexer);

  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_MSG_START);
  assert(token.partial == 0);

  len -= token.end - buf;
  buf = token.end;
  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_METHOD);
  assert(token.partial == 0);
  expect_eq(req->method, token);

  len -= token.end - buf;
  buf = token.end;
  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_URL);
  assert(token.partial == 0);
  expect_eq(req->request_url, token);

  for (;;) {
    /* field */
    len -= token.end - buf;
    buf = token.end;
    token = hl_execute(&lexer, buf, len);

    if (token.kind != HL_FIELD) break;

    assert(token.kind == HL_FIELD);
    assert(token.partial == 0);
    expect_eq(req->headers[num_headers][0], token);

    /* value */
    len -= token.end - buf;
    buf = token.end;
    token = hl_execute(&lexer, buf, len);
    assert(token.kind == HL_VALUE);
    assert(token.partial == 0);
    expect_eq(req->headers[num_headers][1], token);

    num_headers++;
  }

  assert(token.kind == HL_HEADER_END);
  assert(token.partial == 0);

  /* Check HTTP version matches */
  assert(lexer.version_major == req->http_major);
  assert(lexer.version_minor == req->http_minor);

  /* Now read the body */
  for (;;) {
    len -= token.end - buf;
    buf = token.end;
    token = hl_execute(&lexer, buf, len);

    if (token.kind != HL_BODY) break;

    assert(token.kind == HL_BODY);
    assert(token.partial == 0);

    chunk_len = token.end - token.start;
    memcpy(body + body_read, token.start, chunk_len);
    body_read += chunk_len;
  }

  /* match trailing headers */
  if (num_headers < req->num_headers) {
    /* the last token read should have been a HL_FIELD */
    assert(token.kind == HL_FIELD);
    assert(token.partial == 0);
    expect_eq(req->headers[num_headers][0], token);

    /* value */
    len -= token.end - buf;
    buf = token.end;
    token = hl_execute(&lexer, buf, len);
    assert(token.kind == HL_VALUE);
    assert(token.partial == 0);
    expect_eq(req->headers[num_headers][1], token);

    num_headers++;

    for (;;) {
      /* field */
      len -= token.end - buf;
      buf = token.end;
      token = hl_execute(&lexer, buf, len);

      if (token.kind != HL_FIELD) break;

      assert(token.kind == HL_FIELD);
      assert(token.partial == 0);
      expect_eq(req->headers[num_headers][0], token);

      /* value */
      len -= token.end - buf;
      buf = token.end;
      token = hl_execute(&lexer, buf, len);
      assert(token.kind == HL_VALUE);
      assert(token.partial == 0);
      expect_eq(req->headers[num_headers][1], token);

      num_headers++;
    }
  }

  assert(num_headers == req->num_headers);

  assert(body_len == body_read);
  assert(strcmp(body, req->body) == 0);
  printf("bodys match. body_len = %d\n", body_len);

  assert(token.kind == HL_MSG_END);

  /* After the message should either get HL_EOF or HL_EAGAIN depending on
   * req->should_keep_alive. This corresponds roughly to the setting of the
   * Connection header. (HTTP pipelining involves the protocol version too.)
   */

  len -= token.end - buf;
  buf = token.end;
  token = hl_execute(&lexer, buf, len);
  if (req->upgrade) {
    assert(token.kind == HL_EOF);
    token.end = buf + len;
    expect_eq(req->upgrade, token);
  } else if (req->should_keep_alive) {
    assert(token.kind == HL_EAGAIN);
  } else {
    assert(token.kind == HL_EOF);
  }
}


void test_pipeline(const struct message* req1,
                   const struct message* req2,
                   const struct message* req3) {
  hl_lexer lexer;
  hl_token token;
  const size_t len1 = strlen(req1->raw);
  const size_t len2 = strlen(req2->raw);
  const size_t len3 = strlen(req3->raw);
  size_t len = len1 + len2 + len3;
  char raw[len];
  const char* buf = raw;

  memcpy(raw, req1->raw, len1);
  memcpy(raw + len1, req2->raw, len2);
  memcpy(raw + len1 + len2, req3->raw, len3);

  assert(req1->should_keep_alive);
  assert(req2->should_keep_alive);

  hl_req_init(&lexer);

  enum {
    START,
    MSG,
  } state = START;
  int msg_num = 1;

  while (msg_num <= 3) {
    token = hl_execute(&lexer, buf, len);

    switch (state) {
      case START:
        assert(token.kind == HL_MSG_START);
        state = MSG;
        break;

      case MSG:
        if (token.kind == HL_MSG_END) {
          state = START;
          msg_num++;
        }
        break;
    }

    len -= token.end - buf;
    buf = token.end;
  }

  assert(msg_num == 4);
}


void manual_test_CURL_GET() {
  hl_lexer lexer;
  hl_token token;
  const struct message* r = &requests[CURL_GET];
  int raw_len =  strlen(r->raw);
  int len = raw_len;
  const char* buf = r->raw;

  hl_req_init(&lexer);

  /* HL_MSG_START */
  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_MSG_START);
  assert(token.partial == 0);
  assert(token.start == token.end);
  assert(token.end == r->raw);

  /* Method = "GET" */
  len -= token.end - buf;
  buf = token.end;
  assert(len == raw_len);
  assert(buf == r->raw);
  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_METHOD);
  assert(token.partial == 0);
  expect_eq(r->method, token);

  /* HL_URL = "/test" */
  len -= token.end - buf;
  buf = token.end;
  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_URL);
  assert(token.partial == 0);
  expect_eq(r->request_url, token);

  /* HL_FIELD = "User-Agent" */
  len -= token.end - buf;
  buf = token.end;
  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_FIELD);
  assert(token.partial == 0);
  expect_eq(r->headers[0][0], token);

  assert(lexer.version_major == r->http_major);
  assert(lexer.version_minor == r->http_minor);

  /* HL_VALUE = "curl/7.18.0 (i486-pc-linux-gnu) libcurl/7.18.0 OpenSSL/0.9.8g zlib/1.2.3.3 libidn/1.1" */
  len -= token.end - buf;
  buf = token.end;
  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_VALUE);
  assert(token.partial == 0);
  expect_eq(r->headers[0][1], token);

  /* HL_FIELD = "Host" */
  len -= token.end - buf;
  buf = token.end;
  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_FIELD);
  assert(token.partial == 0);
  expect_eq(r->headers[1][0], token);

  /* HL_VALUE = "0.0.0.0=5000" */
  len -= token.end - buf;
  buf = token.end;
  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_VALUE);
  assert(token.partial == 0);
  expect_eq(r->headers[1][1], token);

  /* HL_FIELD = "Accept" */
  len -= token.end - buf;
  buf = token.end;
  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_FIELD);
  assert(token.partial == 0);
  expect_eq(r->headers[2][0], token);

  // HL_VALUE = "*/*"
  len -= token.end - buf;
  buf = token.end;
  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_VALUE);
  assert(token.partial == 0);
  expect_eq(r->headers[2][1], token);

  // HL_HEADER_END
  len -= token.end - buf;
  buf = token.end;
  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_HEADER_END);
  assert(token.start == token.end);
  // CURL_GET has not body, so the headers should be at the end of the buffer.
  assert(token.end == r->raw + raw_len);

  // HL_MSG_END
  len -= token.end - buf;
  buf = token.end;
  token = hl_execute(&lexer, buf, len);
  assert(token.kind == HL_MSG_END);
  assert(token.partial == 0);
  assert(token.start == token.end);
  // We're at the end of the buffer
  assert(token.end == r->raw + raw_len);

  // If we call hl_execute() again, we get HL_EAGAIN.
  token = hl_execute(&lexer, token.end, 0);
  assert(token.kind == HL_EAGAIN);
}


int main() {
  int i, j, k;

  printf("sizeof(hl_token) = %d\n", (int)sizeof(hl_token));
  printf("sizeof(hl_lexer) = %d\n", (int)sizeof(hl_lexer));

  manual_test_CURL_GET();

  for (i = 0; requests[i].name; i++) {
    printf("test_req(%d, %s)\n", i, requests[i].name);
    test_req(&requests[i]);
  }

  for (i = 0; requests[i].name && requests[i].should_keep_alive; i++) {
    for (j = 0; requests[j].name && requests[j].should_keep_alive; j++) {
      for (k = 0; requests[k].name; k++) {
        printf("test_pipeline(%s, %s, %s)\n",
               requests[i].name,
               requests[j].name,
               requests[k].name);
        test_pipeline(&requests[i], &requests[j], &requests[k]);
      }
    }
  }

  return 0;
}
