FROM python:3.12-slim
ENV PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 BIND=0.0.0.0 DB_PATH=/data/hvac.sqlite
WORKDIR /app
COPY app.py mqtt_bridge.py requirements-mqtt.txt /app/
COPY static /app/static
RUN pip install --no-cache-dir --only-binary=:all: -r requirements-mqtt.txt
RUN useradd --uid 10001 --create-home hvac && mkdir /data && chown hvac:hvac /data
USER 10001:10001
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8080/healthz', timeout=2)"
CMD ["python", "app.py"]
