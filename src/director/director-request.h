#ifndef DIRECTOR_REQUEST_H
#define DIRECTOR_REQUEST_H

struct director;
struct director_request;

typedef void
director_request_callback(const struct mail_host *host, const char *hostname,
			  unsigned int username_hash, const char *errormsg,
			  void *context);

void director_request(struct director *dir, const char *username,
		      const char *tag,
		      director_request_callback *callback, void *context);
bool director_request_continue(struct director_request *request);

#endif
