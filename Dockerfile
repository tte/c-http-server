FROM ubuntu:14.04
RUN apt-get update && apt-get install -y \
  screen \
  vim \
  gcc \
  make \
  strace \
  man \
  curl \
  parallel 

COPY . /tmp/
CMD /bin/bash -l 

EXPOSE 80 443

