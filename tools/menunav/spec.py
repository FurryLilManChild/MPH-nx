# Screen specs. A target may name a second capture (5th field): the blinking state of the same button.
# Screen specs. Rects are DS bottom-screen pixels, inclusive. 'src' = capture the fingerprint comes from.
# kind: 'nav' (port highlight), 'popup' (highlight, B = back target), 'game' (A -> DS A), 'starmap', 'files' (A taps the game's red-ringed file), 'play' (cockpit, not a menu)
SCREENS = [
 dict(name='exitsure', kind='popup', src='012', keys=[], back='no', default='yes',
      targets=[('yes',(55,166,87,178),True),('no',(119,166,151,178),True)]),
 dict(name='savesure', kind='popup', src='010', keys=[], back='no', default='yes',
      targets=[('yes',(55,67,86,83),True),('no',(119,67,150,83),True)]),
 dict(name='saveq', kind='popup', src='006', keys=[], back='no', default='yes',
      targets=[('yes',(80,112,111,128),True),('no',(144,112,175,128),True)]),
 dict(name='saveok', kind='popup', src='007', keys=[], back='ok', default='ok',
      targets=[('ok',(112,121,143,136),True)]),
 dict(name='launchsure', kind='popup', src='016', keys=[], back='no', default='yes',
      targets=[('yes',(152,162,183,178),True),('no',(217,162,247,178),True)]),
 dict(name='enter', kind='popup', src='005', keys=[], back='no', default='yes',
      targets=[('yes',(80,134,111,149),True),('no',(144,134,175,149),True)]),
 dict(name='scan', kind='popup', src=None, keys=[], back=None, default='right',
      targets=[('left',(56,134,87,149),True,'014'),('ok',(113,134,144,149),True,'014'),('right',(169,133,200,148),True,'013','105')]),
 dict(name='landed', kind='nav', src='004', keys=[(62,2,142,38),(62,143,143,181),(0,33,46,68),(203,57,255,92)], back=None, default='save',
      targets=[('save',(62,2,142,38),False),('weapon',(0,33,46,68),False),('logbook',(203,57,255,92),False),
               ('options',(0,113,46,148),False),('exit',(62,143,143,181),False),('launch',(150,121,250,183),False)]),
 dict(name='options', kind='nav', src='011', keys=[(195,0,246,28),(14,40,90,50)], back='back', default='styR',
      targets=[('back',(195,0,246,28),False),('styR',(17,55,78,82),False),('styL',(20,86,76,111),False),
               ('dualR',(20,115,76,139),False),('dualL',(20,144,76,168),False),('slider',(103,59,140,123),False),
               ('invOn',(104,160,116,172),False),('invOff',(163,162,175,174),False)]),
 dict(name='weapons', kind='nav', src='008', keys=[(195,85,245,112),(198,148,245,166)], back='back', default='slot1',
      targets=[('back',(195,85,245,112),False),('slot1',(33,77,102,131),False)]),
 dict(name='logbooklist', kind='nav', src='015', keys=[(5,29,55,56),(10,70,55,82),(215,81,239,159)], back='back', default='row1',
      targets=[('back',(5,29,55,56),False),('c1',(73,31,104,61),False),('c2',(110,33,137,59),False),
               ('c3',(145,33,172,59),False),('c4',(204,33,231,59),False),
               ('row1',(65,83,208,97),False),('row2',(65,98,208,112),False),('row3',(65,113,208,127),False),
               ('row4',(65,128,208,142),False),('row5',(65,143,208,157),False),
               ('top',(215,81,239,95),False),('up',(215,97,239,118),False),('down',(215,121,239,142),False),('bottom',(215,144,239,159),False)]),
 dict(name='logbook', kind='nav', src='009', keys=[(5,29,55,56),(10,70,55,82)], back='back', default='c1',
      targets=[('back',(5,29,55,56),False),('c1',(75,33,102,59),True),('c2',(110,33,137,59),True),
               ('c3',(145,33,172,59),True),('c4',(204,33,231,59),True)]),
 dict(name='starmap', kind='starmap', src='003', keys=[(80,2,250,12),(81,16,248,24)], back=None, default='land',
      targets=[('land',(0,137,71,183),False)]),
 # File select: keyed on the letters A and C on their orange circles (the background behind the
 # back arrow and the Movies icon changes colour all the time; native19 captures).
 # The game draws a red ring on the highlighted file; Switch A taps that file (DS A here opens Movies).
 dict(name='files', kind='files', src='002', keys=[(40,40,56,54),(200,40,216,54)], back=None, default='fileB',
      targets=[('fileA',(14,38,82,106),False),('fileB',(94,38,162,106),False),('fileC',(174,38,242,106),False)]),
 dict(name='title', kind='game', src='001', keys=[(8,160,31,184),(216,110,240,128)], back=None, default=None, targets=[]),
 # In-game play screen (cockpit frame): not a menu. Used by the Top only + HUD layout and the
 # D-pad weapon controls. Last, so every pop-up drawn on the same frame wins.
 dict(name='play', kind='play', src='101', keys=[(2,2,62,12),(204,16,248,60)], back=None, default=None, targets=[]),
]
