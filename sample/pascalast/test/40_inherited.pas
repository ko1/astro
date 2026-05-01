program InheritedTest;
type
  TBase = class
    val: integer;
    constructor Create(v: integer);
    procedure show; virtual;
    destructor Done;
  end;

  TDerived = class(TBase)
    extra: integer;
    constructor Create(v, e: integer);
    procedure show; override;
  end;

constructor TBase.Create(v: integer);
begin
  Self.val := v;
  writeln('TBase.Create v=', v)
end;

procedure TBase.show;
begin
  writeln('TBase.show val=', Self.val)
end;

destructor TBase.Done;
begin
  writeln('TBase.Done bye val=', Self.val)
end;

constructor TDerived.Create(v, e: integer);
begin
  inherited Create(v);            { calls TBase.Create with self }
  Self.extra := e;
  writeln('TDerived.Create extra=', e)
end;

procedure TDerived.show;
begin
  inherited show;                 { calls TBase.show with self }
  writeln('TDerived.show extra=', Self.extra)
end;

var d: TDerived;
begin
  d := TDerived.Create(10, 99);
  d.show;
  d.Done
end.
