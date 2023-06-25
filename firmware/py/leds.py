import time
import machine


class leds() :
    _leds = [
        (True, False, None, None),
        (False, True, None, None),
        (None, True, False, None),
        (None, False, True, None),
        (False, None, True, None),
        (None, None, False, True),
        (None, False, None, True),
        (False, None, None, True),
        (True, None, False, None)
    ]

    _lines = [
        machine.Pin(6, machine.Pin.OUT),
        machine.Pin(5, machine.Pin.OUT),
        machine.Pin(4, machine.Pin.OUT),
        machine.Pin(3, machine.Pin.OUT),
    ]

    def __init__(self) -> None:
        self.count = 0
        self.reverse = False

    def do_loop_step(self) -> None:
        for state, lineNo in zip(self._leds[count], range(len(self._lines))):
            if state == None:
                self._lines[lineNo].init(mode=machine.Pin.IN)
            else:
                self._lines[lineNo].init(mode=machine.Pin.OUT)
                if state is True:
                    self._lines[lineNo].high()
                else:
                    self._lines[lineNo].low()

        if self.reverse:
            count = (count-1) % len(self._leds)
        else:
            count = (count+1) % len(self._leds)

