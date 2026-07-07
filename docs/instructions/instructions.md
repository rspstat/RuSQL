1. C++ 빌드
```
cmake -S code -B code/build
cmake --build code/build --config Debug

code/build/backend/tests/Debug/engine_tests.exe
code/build/backend/cli/Debug/engine_cli.exe
code/build/backend/server/Debug/engine_server.exe
```

<br/>

2. UI 실행
```
cd code/frontend
npm install
npm run tauri dev
```

<br/>

3. Git
```
git checkout main
git checkout cpptest

git add -A
git commit -m "메시지"
git push

git checkout -b 브랜치이름
git add -A
git commit -m "메시지"
git push -u origin 브랜치이름
```
