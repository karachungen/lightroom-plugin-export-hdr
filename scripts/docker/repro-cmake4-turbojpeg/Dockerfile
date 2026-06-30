# Reproduce the CMake 4 policy failure seen when libultrahdr vendors libjpeg-turbo 3.0.1.
# Linux-only; does not run the full MSVC Windows bundle.
FROM python:3.12-slim-bookworm

RUN apt-get update && apt-get install -y --no-install-recommends git ca-certificates \
	&& rm -rf /var/lib/apt/lists/* \
	&& pip install --no-cache-dir cmake==4.0.2

WORKDIR /work
RUN git clone --depth 1 --branch 3.0.1 https://github.com/libjpeg-turbo/libjpeg-turbo.git turbo

COPY run-in-container.sh /work/run-in-container.sh
RUN chmod +x /work/run-in-container.sh

ENTRYPOINT ["/work/run-in-container.sh"]
