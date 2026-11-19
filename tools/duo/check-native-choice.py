"""Check the visible Duet choice page: python check-native-choice.py SERIAL 'Game title'."""
import subprocess
import sys
import xml.etree.ElementTree as ET

serial, title = sys.argv[1:]
xml = subprocess.check_output(['adb', '-s', serial, 'exec-out', 'uiautomator', 'dump', '--windows', '/dev/tty'], timeout=20)
page = ET.fromstring(xml[xml.index(b'<?xml'):xml.index(b'</displays>') + len(b'</displays>')])
views = {n.get('resource-id').split(':id/')[-1]: n for n in page.iter('node')
         if n.get('package') in ('com.tapionallanaire.duetplus', 'com.tapionallanaire.duetplus.dev')}
assert views['game_title'].get('text') == title, 'Wrong game page'
for action in ('primary', 'secondary'):
    assert views[action].get('enabled') == 'true', views['summary'].get('text')
print(title, views['availability'].get('text'), views['primary'].get('text'), sep=' | ')
