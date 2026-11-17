FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app

COPY server.py web_admin.py device_status.py ./
COPY web ./web

RUN mkdir -p /data

EXPOSE 9000 8765

CMD ["python", "server.py", "--host", "0.0.0.0", "--port", "9000", "--http-host", "0.0.0.0", "--http-port", "8765", "--database", "/data/recycling.db"]
