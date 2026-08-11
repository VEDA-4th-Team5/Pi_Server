pipeline {
    agent { label 'pi-native' }

    options {
        buildDiscarder(logRotator(numToKeepStr: '10'))
        timeout(time: 30, unit: 'MINUTES')
    }

    triggers {
        pollSCM('H/5 * * * *')
    }

    stages {
        stage('Build') {
            steps {
                sh 'cmake -S . -B cmake-build'
                sh 'cmake --build cmake-build -j2'
            }
        }
        stage('Test') {
            steps {
                sh 'ctest --test-dir cmake-build --output-on-failure'
            }
        }
    }
}
