FROM alpine:3.23 as builder

RUN apk update \
    && apk add alpine-sdk cmake linux-headers

WORKDIR /ahp

COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build

FROM alpine:3.23

RUN apk update && apk add libgcc libstdc++

COPY --from=builder /ahp/build/tcp-relay /usr/local/bin/tcp-relay
