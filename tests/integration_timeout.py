import os,sys,time,tempfile,subprocess,pathlib,shutil
import dbus,dbus.service,dbus.mainloop.glib
from gi.repository import GLib
IF='com.cpis.engine'
if len(sys.argv) == 1:
 sys.exit(subprocess.call(['dbus-run-session', '--', sys.executable, __file__, 'private']))
LIBRARY = os.environ.get('SOGOU_TEST_LIBRARY', str(pathlib.Path(__file__).resolve().parents[1] / 'build/libsogou-native.so'))
if len(sys.argv)>1 and sys.argv[1]=='mock':
 dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
 class Engine(dbus.service.Object):
  def __init__(self,bus):
   self.name=dbus.service.BusName(IF,bus);super().__init__(bus,'/com/cpis/engine');self.raw='';self.commit='';self.stall=False;self.sets=0
  @dbus.service.method(IF,in_signature='sss',out_signature='i')
  def SetMode(self,u,m,l): self.sets+=1;return 0
  @dbus.service.method(IF,in_signature='s',out_signature='i')
  def Clear(self,u): self.raw='';self.commit='';return 0
  @dbus.service.method(IF,in_signature='sai',out_signature='ai')
  def PushChars(self,u,chars):
   if self.stall: self.stall=False;time.sleep(.55)
   self.raw+=''.join(chr(c) for c in chars);return [0]*len(chars)
  @dbus.service.method(IF,in_signature='s',out_signature='asasssi')
  def AcquireResult(self,u):
   c=self.commit;self.commit='';return (['你好'] if self.raw else [],[],self.raw,c,0)
  @dbus.service.method(IF,in_signature='sii',out_signature='i')
  def SelectCandidate(self,u,t,i): self.raw='';self.commit='你好';return 0
  @dbus.service.method(IF,in_signature='',out_signature='')
  def Stall(self): self.stall=True
  @dbus.service.method(IF,in_signature='',out_signature='i')
  def SetCount(self): return self.sets
 e=Engine(dbus.SessionBus());GLib.MainLoop().run();sys.exit()
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus=dbus.SessionBus();children=[]
with tempfile.TemporaryDirectory(prefix='sogou-test-') as tmp:
 try:
  base=pathlib.Path(tmp); conf=base/'config/fcitx5';conf.mkdir(parents=True)
  (conf/'profile').write_text('[Groups/0]\nName=Default\nDefault Layout=us\nDefaultIM=sogou-native\n[Groups/0/Items/0]\nName=keyboard-us\n[Groups/0/Items/1]\nName=sogou-native\n[GroupOrder]\n0=Default\n')
  addons=base/'data/fcitx5/addon';addons.mkdir(parents=True)
  (addons/'sogou-native.conf').write_text(f'[Addon]\nName=Sogou Native\nCategory=InputMethod\nVersion=0.1\nLibrary={LIBRARY.removesuffix(".so")}\nType=SharedLibrary\nOnDemand=True\n')
  ims=base/'data/fcitx5/inputmethod';ims.mkdir(parents=True)
  shutil.copy(pathlib.Path(__file__).resolve().parents[1]/'data/sogou-native-inputmethod.conf',ims/'sogou-native.conf')
  env=dict(os.environ,XDG_CONFIG_HOME=str(base/'config'),XDG_DATA_HOME=str(base/'data'),SOGOU_NATIVE_OFFLINE='1',DISPLAY='',WAYLAND_DISPLAY='')
  children.append(subprocess.Popen(['/usr/bin/python3',__file__,'mock']))
  log=open(base/'fcitx.log','w+')
  children.append(subprocess.Popen(['fcitx5','-D','--disable','all','--enable','dbus,dbusfrontend,sogou-native,keyboard'],env=env,stdout=log,stderr=log))
  for _ in range(100):
   if bus.name_has_owner('org.fcitx.Fcitx5') and bus.name_has_owner(IF):break
   time.sleep(.05)
  assert bus.name_has_owner('org.fcitx.Fcitx5'), 'test fcitx failed to start'
  im=dbus.Interface(bus.get_object('org.fcitx.Fcitx5','/org/freedesktop/portal/inputmethod'),'org.fcitx.Fcitx.InputMethod1')
  path,_=im.CreateInputContext([('program','sogou-regression')])
  obj=bus.get_object('org.fcitx.Fcitx5',path)
  ic=dbus.Interface(obj,'org.fcitx.Fcitx.InputContext1')
  control=dbus.Interface(bus.get_object('org.fcitx.Fcitx5','/controller'),'org.fcitx.Fcitx.Controller1')
  engine=dbus.Interface(bus.get_object(IF,'/com/cpis/engine'),IF)
  commits=[];preedits=[]
  ic.connect_to_signal('CommitString',lambda x:commits.append(str(x)))
  ic.connect_to_signal('UpdateClientSideUI',lambda p,*args:preedits.append(''.join(str(x[0]) for x in p)))
  def pump(seconds=.02):
   end=time.monotonic()+seconds
   ctx=GLib.MainContext.default()
   while time.monotonic()<end:
    while ctx.pending():ctx.iteration(False)
    time.sleep(.002)
  def key(k):
   t=time.monotonic();handled=ic.ProcessKeyEvent(ord(k) if isinstance(k,str) else k,0,0,False,0);elapsed=time.monotonic()-t
   pump();return elapsed
  ic.SetCapability(dbus.UInt64(2|16|(1<<39)));ic.FocusIn();control.SetCurrentIM('sogou-native');control.Activate();pump()
  assert control.CurrentInputMethod()=='sogou-native'
  for _ in range(10):
   for c in 'nihao':key(c)
   key(' ')
  assert commits==['你好']*10,commits
  assert engine.SetCount()==1,engine.SetCount()
  commits.clear();engine.Stall()
  times=[key(c) for c in 'nihao']
  assert not commits,commits
  assert preedits[-1]=='nihao',preedits[-5:]
  assert max(times)<.3,times
  assert max(times[1:])<.05,times
  pump(1.3);key(' ')
  assert commits==['你好'],commits
  print('PASS: 10 normal words, one mode initialization; stalled nihao preserved and recovered; key times ms:',[round(x*1000,1) for x in times])
  commits.clear();engine.Stall();key('n');key(0xff08);pump(1.3)
  assert not commits,commits
  assert preedits[-1]=='',preedits[-5:]
  for c in 'nihao':key(c)
  key(' ');assert commits==['你好'],commits
  commits.clear();engine.Stall();key('n');key(0xff1b);pump(1.3)
  assert not commits and preedits[-1]=='',(commits,preedits[-5:])
  print('PASS: backspace and Escape during timeout retain/cancel text correctly')
  ic.DestroyIC()
  log.flush();log.seek(0);diagnostics=log.read()
  assert 'event=rpc_failed' in diagnostics and 'reason=engine_reply_timeout' in diagnostics
  assert 'method=PushChars' in diagnostics and 'event=recovery_ok' in diagnostics
  assert 'event=operation' in diagnostics and 'kind=key' in diagnostics
  assert 'nihao' not in diagnostics and '你好' not in diagnostics
  print('PASS: timeout method, cause, duration and recovery logged without input text')
 finally:
  for p in reversed(children):
   p.terminate()
   try:p.wait(timeout=3)
   except subprocess.TimeoutExpired:p.kill();p.wait()
