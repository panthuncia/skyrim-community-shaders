"""cpuload.CpuLoad against fake system and process times."""

from cstest import cpuload


class FakeTimes:
    """Advances one second a sample over four CPUs: 4e7 units of total time."""

    def __init__(self):
        self.idle = 0
        self.total = 0
        self.game = 0

    def step(self, busy, game):
        self.total += 4e7
        self.idle += 4e7 * (1 - busy)
        self.game += 4e7 * game


def load_with(times, tools=None):
    return cpuload.CpuLoad(cpus=4, system=lambda: (times.idle, times.total), process=lambda pid: times.game,
                           tools=lambda: tools or {"cl.exe": 3, "ninja.exe": 1})


def test_counts_saturated_world_seconds():
    times = FakeTimes()
    load = load_with(times)
    load.sample(1, True)
    for _ in range(6):  # every CPU busy, the game on a fifth of them
        times.step(1.0, 0.2)
        load.sample(1, True)
    times.step(0.5, 0.2)  # a calm second ends the stretch
    load.sample(1, True)
    result = load.result()
    assert result["saturated_s"] == 6.0
    assert result["longest_s"] == 6.0
    assert result["others_max_cpus"] == 3.2
    assert result["tools"] == {"cl.exe": 3, "ninja.exe": 1}
    assert "saturated by other processes 6 s" in cpuload.text(result)
    assert "cl.exe x3, ninja.exe" in cpuload.text(result)


def test_the_game_itself_is_not_saturation():
    times = FakeTimes()
    load = load_with(times)
    load.sample(1, True)
    for _ in range(10):  # every CPU busy, the game on most of them
        times.step(1.0, 0.8)
        load.sample(1, True)
    assert load.result()["saturated_s"] == 0.0
    assert cpuload.text(load.result()) == ""


def test_only_world_seconds_count():
    times = FakeTimes()
    load = load_with(times)
    load.sample(1, False)
    for _ in range(10):
        times.step(1.0, 0.1)
        load.sample(1, False)
    assert load.result()["saturated_s"] == 0.0


def test_a_new_game_restarts_the_deltas():
    times = FakeTimes()
    load = load_with(times)
    load.sample(1, True)
    times.step(1.0, 0.1)
    load.sample(2, True)  # another PID: no delta across it
    assert load.result()["saturated_s"] == 0.0
    times.step(1.0, 0.1)
    load.sample(2, True)
    assert load.result()["saturated_s"] == 1.0


def test_short_saturation_is_not_reported():
    assert cpuload.text({"saturated_s": 3, "cpus": 4, "others_max_cpus": 3.5, "tools": {}}) == ""
