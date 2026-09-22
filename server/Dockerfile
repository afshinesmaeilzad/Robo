# Small, fast image: dependencies are installed in their own layer, so code
# changes rebuild in seconds.
FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 PIP_NO_CACHE_DIR=1
WORKDIR /app

COPY requirements.txt .
RUN pip install -r requirements.txt

COPY *.py ./
# Notes and snapshots live on a volume, so they survive a rebuild
VOLUME /app/data
EXPOSE 8000

CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8000"]
