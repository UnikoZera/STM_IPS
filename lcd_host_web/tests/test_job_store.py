import time

from lcd_host_web.job_store import ConversionJobStore


def test_conversion_job_lifecycle():
    store = ConversionJobStore()
    job_id = store.submit(lambda context: {"ok": True})
    deadline = time.time() + 2
    while time.time() < deadline:
        job = store.get(job_id)
        if job and job["status"] == "completed":
            assert job["result"] == {"ok": True}
            assert job["progress"] == 100.0
            return
        time.sleep(0.01)
    raise AssertionError("job did not complete")


def test_cancelled_job_does_not_publish_result():
    store = ConversionJobStore()

    def worker(context):
        while not context.cancelled:
            time.sleep(0.01)
        raise RuntimeError("cancelled")

    job_id = store.submit(worker)
    time.sleep(0.02)
    assert store.cancel(job_id)
    deadline = time.time() + 2
    while time.time() < deadline:
        job = store.get(job_id)
        if job and job["status"] == "cancelled":
            assert job["result"] is None
            return
        time.sleep(0.01)
    raise AssertionError("job was not cancelled")


def test_progress_snapshot_is_monotonic_and_includes_frame_details():
    store = ConversionJobStore()

    def worker(context):
        context.total_frames = 120
        context.report(42, 50, "converting", "正在转换：已处理 50/120 帧")
        context.report(20, 20, "converting", "stale")
        time.sleep(0.03)
        return {"ok": True}

    job_id = store.submit(worker)
    deadline = time.time() + 2
    while time.time() < deadline:
        job = store.get(job_id)
        if job and job["status"] == "completed":
            assert job["progress"] == 100.0
            assert job["phase"] == "completed"
            assert job["total_frames"] == 120
            assert job["processed_frames"] == 50
            return
        time.sleep(0.01)
    raise AssertionError("job did not complete")
