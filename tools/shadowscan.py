# Heuristic MSVC shadowing scan (C4456/C4457/C4458): python3 tools/shadowscan.py Source
# Reports locals/params that hide class members (incl. common engine base members) or outer locals.
# Known false positives: lambda params typed TPair<double, ...> show up as "double"/"float".
import re,sys,os,glob
ROOT=sys.argv[1]
ENGINE={
'UObject':set(),'UActorComponent':set('PrimaryComponentTick ComponentTags AssetUserData bReplicates bAutoActivate bIsActive bCanEverAffectNavigation bWantsInitializeComponent bAutoRegister bTickInEditor CreationMethod WorldPrivate OwnerPrivate bRegistered bRenderStateCreated'.split()),
'AActor':set('PrimaryActorTick bHidden bNetTemporary bOnlyRelevantToOwner bAlwaysRelevant bReplicateMovement bTearOff bNetLoadOnClient bNetUseOwnerRelevancy bBlockInput bCanBeDamaged InputComponent RemoteRole Role Owner NetDormancy SpawnCollisionHandlingMethod AutoReceiveInput InputPriority NetCullDistanceSquared NetTag NetUpdateFrequency MinNetUpdateFrequency NetPriority Instigator Children RootComponent InitialLifeSpan CustomTimeDilation Layers ParentComponent Tags OnTakeAnyDamage OnTakePointDamage OnTakeRadialDamage OnActorBeginOverlap OnActorEndOverlap OnActorHit OnDestroyed OnEndPlay ReplicatedMovement AttachmentReplication TimerHandle_LifeSpanExpired LastRenderTime bReplicates bActorIsBeingDestroyed bHasFinishedSpawning bActorInitialized bTickFunctionsRegistered NetDriverName'.split()),
'APawn':set('bUseControllerRotationPitch bUseControllerRotationYaw bUseControllerRotationRoll AutoPossessPlayer AutoPossessAI AIControllerClass BaseEyeHeight Controller PreviousController ControlInputVector LastControlInputVector PlayerState LastHitBy RemoteViewPitch bInputEnabled'.split()),
'AController':set('PlayerState StartSpot ControlRotation bAttachToPawn bIsPlayerController StateName Pawn Character TransformComponent OnInstigatedAnyDamage OnPossessedPawnChanged OldPawn'.split()),
'APlayerController':set('Player AcknowledgedPawn MyHUD PlayerCameraManager PlayerCameraManagerClass bAutoManageActiveCameraTarget TargetViewRotation SmoothTargetViewRotationSpeed HiddenActors HiddenPrimitiveComponents ClientCap CheatManager CheatClass PlayerInput ActiveForceFeedbackEffects bPlayerIsWaiting NetPlayerIndex PendingSwapConnection NetConnection RotationInput InputYawScale InputPitchScale InputRollScale bShowMouseCursor bEnableClickEvents bEnableTouchEvents bEnableMouseOverEvents bEnableTouchOverEvents bForceFeedbackEnabled ForceFeedbackScale ClickEventKeys DefaultMouseCursor CurrentMouseCursor DefaultClickTraceChannel CurrentClickTraceChannel HitResultTraceDistance SeamlessTravelCount InactiveStateInputComponent CurrentTouchInterface SpectatorPawn bIsLocalPlayerController SpawnLocation CurrentInputStack'.split()),
'APlayerState':set('Score PlayerId Ping CompressedPing bIsSpectator bOnlySpectator bIsABot bIsInactive bFromPreviousLevel StartTime EngineMessageClass SavedNetworkAddress UniqueId PlayerNamePrivate OldNamePrivate PawnPrivate'.split()),
'AGameStateBase':set('GameModeClass AuthorityGameMode SpectatorClass PlayerArray ServerWorldTimeSecondsDelta ReplicatedWorldTimeSeconds ReplicatedWorldTimeSecondsDouble bReplicatedHasBegunPlay'.split()),
'AGameModeBase':set('OptionsString GameSessionClass GameStateClass PlayerControllerClass PlayerStateClass HUDClass DefaultPawnClass SpectatorClass ReplaySpectatorPlayerControllerClass ServerStatReplicatorClass GameSession GameState ServerStatReplicator DefaultPlayerName bUseSeamlessTravel bStartPlayersAsSpectators bPauseable'.split()),
'AHUD':set('PlayerOwner bLostFocusPaused bShowHUD bShowDebugInfo bShowHitBoxDebugInfo bShowOverlays bEnableDebugTextShadow PostRenderedActors DebugDisplay ToggledDebugCategories Canvas DebugCanvas DebugTextList ShowDebugTargetDesiredClass ShowDebugTargetActor HitBoxMap HitBoxHits CurrentHitBoxHits'.split()),
'UGameInstanceSubsystem':set(),'UWorldSubsystem':set(),'USubsystem':set(),
}
ENGINE_BASE={'AActor':'UObject','APawn':'AActor','AController':'AActor','APlayerController':'AController','APlayerState':'AActor','AGameStateBase':'AActor','AGameModeBase':'AActor','AHUD':'AActor','UActorComponent':'UObject'}
KW=set('return if else for while do switch case new delete sizeof throw co_return goto default break continue using namespace typedef static_assert template typename class struct enum'.split())
def strip(src):
    out=[];i=0;n=len(src)
    while i<n:
        c=src[i]
        if src.startswith('//',i):
            j=src.find('\n',i); j=n if j<0 else j; out.append(' '*(j-i)); i=j
        elif src.startswith('/*',i):
            j=src.find('*/',i+2); j=n if j<0 else j+2; out.append(re.sub(r'[^\n]',' ',src[i:j])); i=j
        elif c in '"\'':
            j=i+1
            while j<n and src[j]!=c:
                j+= 2 if src[j]=='\\' else 1
            out.append(c+' '*(j-i-1)+c); i=j+1
        elif c=='#':
            j=src.find('\n',i)
            while j>0 and src[j-1]=='\\': j=src.find('\n',j+1)
            j=n if j<0 else j; out.append(re.sub(r'[^\n]',' ',src[i:j])); i=j
        else: out.append(c); i+=1
    return ''.join(out)
files=[f for f in glob.glob(ROOT+'/**/*.*',recursive=True) if f.endswith(('.h','.cpp'))]
S={f:strip(open(f,encoding='utf-8',errors='replace').read()) for f in files}
# classes
members={};bases={}
def match_brace(s,i):
    d=0
    for j in range(i,len(s)):
        if s[j]=='{': d+=1
        elif s[j]=='}':
            d-=1
            if d==0: return j
    return len(s)
TYPE=r'(?:const\s+)?(?:unsigned\s+)?(?:[A-Za-z_]\w*(?:::\w+)*)(?:\s*<[^;{}]*?>)?(?:\s*(?:\*|&|const\b))*'
for f,s in S.items():
    for m in re.finditer(r'\b(class|struct)\s+(?:\w+_API\s+)?(\w+)\s*(?:final\s*)?(?::\s*(?:public|protected|private)\s+(\w+))?\s*\{',s):
        name=m.group(2); base=m.group(3)
        b=m.end()-1; e=match_brace(s,b); body=s[b+1:e]
        # depth-0 statements
        d=0;flat=[]
        for ch in body:
            if ch=='{': d+=1; 
            if d==0: flat.append(ch)
            if ch=='}': d-=1
        flat=''.join(flat)
        flat=re.sub(r'\bU(PROPERTY|FUNCTION)\s*\((?:[^()]|\([^()]*\))*\)','',flat)
        flat=re.sub(r'\bGENERATED_\w+\s*\(\s*\)','',flat)
        mem=members.setdefault(name,set())
        for st in flat.split(';'):
            st=st.strip()
            st2=re.sub(r'^(public|private|protected)\s*:','',st).strip()
            st2=re.sub(r'^(public|private|protected)\s*:','',st2).strip()
            if '(' in st2.split('=')[0]: continue
            mm=re.match(r'^(?:static\s+|mutable\s+|inline\s+|constexpr\s+)*'+TYPE+r'\s+(\w+)\s*(?:\[[^\]]*\])?\s*(?:=.*|\{.*)?$',st2,re.S)
            if mm and mm.group(1) not in KW:
                mem.add(mm.group(1))
        if base: bases[name]=base
def allmembers(c,seen=None):
    out=set()
    while c:
        out|=members.get(c,set())|ENGINE.get(c,set())
        c=bases.get(c) or ENGINE_BASE.get(c)
    return out
# globals per cpp (namespace-scope vars incl anon namespace)
DECL=re.compile(r'(?<![\w:.>])(?:const\s+|static\s+|constexpr\s+)*(?:auto|bool|int|int8|int16|int32|int64|uint8|uint16|uint32|uint64|float|double|TCHAR|FString|FName|FText|FVector|FVector2D|FRotator|FQuat|FTransform|FColor|FLinearColor|FBox|[TFUAEI][A-Z]\w*)(?:\s*<(?:[^;{}<>]|<[^;{}<>]*>)*>)?(?:\s*(?:\*|&|const\b))*\s+([A-Za-z_]\w*)\s*(?==|;|\(|\{|:(?!:)|\[|,)')
res=[]
for f,s in S.items():
    if not f.endswith(('.cpp','.h')): continue
    # function definitions: Class::Func(...) ... {   or free functions in namespaces
    for m in re.finditer(r'\b(?:(\w+)::)?(~?\w+)\s*\(',s):
        cls,fn=m.group(1),m.group(2)
        if fn in KW or fn in ('if','for','while','switch','catch'): continue
        # find matching paren
        i=m.end()-1;d=0
        for j in range(i,len(s)):
            if s[j]=='(': d+=1
            elif s[j]==')':
                d-=1
                if d==0: break
        params=s[i+1:j]
        rest=s[j+1:j+200]
        mm=re.match(r'\s*(?:const\s*)?(?:override\s*)?(?:final\s*)?(?:noexcept\s*)?(?:->\s*[\w:<>*& ]+)?\s*(?::[^{;]*)?\{',rest,re.S)
        if not mm: continue
        # must be at file/namespace/class scope: check the prefix char context: previous token not '=' ',' '(' 'return'
        pre=s[max(0,m.start()-60):m.start()]
        if re.search(r'(=|\(|,|return|\.|->|!|&&|\|\||\?|:)\s*$',pre) and not re.search(r'\w[\w<>:*&\s]*\s$',pre): continue
        if not re.search(r'[\w>*&]\s+$',pre) and cls is None: continue
        if cls is None:
            # inline member in a class body?  find enclosing class
            enc=None
            for cm in re.finditer(r'\b(class|struct)\s+(?:\w+_API\s+)?(\w+)\s*(?:final\s*)?(?::\s*(?:public|protected|private)\s+\w+)?\s*\{',s[:m.start()]):
                b=cm.end()-1
                if match_brace(s,b)>m.start(): enc=cm.group(2)
            cls=enc
        body_start=j+1+mm.end()-1
        body_end=match_brace(s,body_start)
        mems=allmembers(cls) if cls else set()
        line=s.count('\n',0,body_start)+1
        # params
        pnames=[]
        for p in re.split(r',(?![^<]*>)',params):
            p=p.split('=')[0].strip()
            pm=re.search(r'([A-Za-z_]\w*)\s*(?:\[\])?$',p)
            if pm and len(p.split())>1: pnames.append(pm.group(1))
        for pn in pnames:
            if pn in mems: res.append((f,line,'C4458 param',pn,cls,fn))
        # scan body scopes
        body=s[body_start:body_end+1]
        scopes=[set(pnames)]
        pos=0
        # pre-tokenize declaration hits by position
        hits={}
        for dm in DECL.finditer(body):
            name=dm.group(1)
            if name in KW: continue
            # avoid function calls like 'Foo(x)' preceded by type? require previous non-space char in ; { } ( or start, or ')' for range-for not
            k=dm.start()-1
            while k>=0 and body[k] in ' \t\n': k-=1
            prev=body[k] if k>=0 else '{'
            if prev not in ';{}(:,' : continue
            if prev==':' and k>0 and body[k-1]==':': continue
            if prev==',' :
                # only if inside a parameter list of a lambda: handled by lambda param regex
                continue
            if prev==':' :
                # labels/case: accept only after 'public:' style? skip ternary
                if not re.search(r'(case [^:]*|default)\s*$',body[max(0,k-40):k]): continue
            # skip type names used as expressions, e.g. "FVector X" ok. exclude if followed by '(' and token before name is not a type (e.g. return Foo(..))
            if prev=='(':
                # scoped to the following statement (for/if/while init, lambda/catch params)
                d2=0;q=k
                while q<len(body):
                    if body[q]=='(': d2+=1
                    elif body[q]==')':
                        d2-=1
                        if d2==0: break
                    q+=1
                r=q+1
                while r<len(body) and body[r] in ' \t\n': r+=1
                mmut=re.match(r'(?:mutable\s*)?(?:->\s*[\w:<>*& ]+?)?\s*\{',body[r:])
                hits.setdefault(dm.start(1),('PEND',name,(r+mmut.end()-1) if mmut else -1))
                continue
            hits.setdefault(dm.start(1),name)
        # multiple declarators: "int32 A = 0, B = 0;"
        for dm in re.finditer(r'(?<![\w:])(?:int32|int64|float|double|bool|FVector|FString|FName|uint8)\s+\w+\s*=\s*[^;(){}]*?(?:,\s*(\w+)\s*(?==|;|,))+',body):
            for xm in re.finditer(r',\s*([A-Za-z_]\w*)\s*(?==)',dm.group(0)):
                hits.setdefault(dm.start()+xm.start(1),xm.group(1))
        for lm in re.finditer(r'\]\s*\(([^()]*)\)\s*(?:mutable\s*)?(?:->\s*[\w:<>*& ]+?)?\s*\{',body):
            for pm in re.finditer(r'([A-Za-z_]\w*)\s*(?:,|$)',lm.group(1)):
                seg=lm.group(1)[:pm.start()].split(',')[-1].strip()
                if seg: hits.setdefault(lm.start(1)+pm.start(1),('PEND',pm.group(1),lm.end()-1))
        events=sorted(hits.items())
        ei=0
        # walk braces
        stack=[set()]
        pendings={}
        for idx,ch in enumerate(body):
            while ei<len(events) and events[ei][0]<=idx:
                p,name=events[ei]; ei+=1
                pend=None
                if isinstance(name,tuple): pend=name[2]; name=name[1]
                line2=s.count('\n',0,body_start+int(p))+1
                if name in mems: res.append((f,line2,'C4458 local',name,cls,fn))
                if pend is None:
                    outer=set().union(*scopes)|set().union(*stack[:-1])
                    if name in outer: res.append((f,line2,'C4456/7 outer',name,cls,fn))
                    if name in stack[-1] and False: pass
                    stack[-1].add(name)
                else:
                    outer=set().union(*scopes)|set().union(*stack)|pendings.get(pend,set())
                    if name in outer: res.append((f,line2,'C4456/7 outer',name,cls,fn))
                    if pend>=0: pendings.setdefault(pend,set()).add(name)
            if ch=='{': stack.append(set(pendings.pop(idx,set())))
            elif ch=='}':
                if len(stack)>1: stack.pop()
seen=set()
for r in sorted(res):
    k=(r[0],r[1],r[3])
    if k in seen: continue
    seen.add(k)
    print(f"{os.path.relpath(r[0],ROOT)}:{r[1]}: {r[2]} '{r[3]}' in {r[4]}::{r[5]}")
