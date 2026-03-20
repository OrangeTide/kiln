{ greetunit.pas : greeting unit }
unit greetunit;

interface

procedure Greet(const Name: string);
procedure Farewell(const Name: string);

implementation

procedure Greet(const Name: string);
begin
  WriteLn('Hello, ', Name, '!');
end;

procedure Farewell(const Name: string);
begin
  WriteLn('Goodbye, ', Name, '!');
end;

end.
