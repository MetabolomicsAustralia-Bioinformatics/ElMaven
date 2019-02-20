#include "videoplayer.h"

#include <QtWidgets>
#include <QVideoWidget>
#include <QSizePolicy>
#include <QSettings>
#include <QPushButton>

VideoPlayer::VideoPlayer(QSettings *settings,QWidget* parent)
    : QWidget(parent)
    , m_settings(settings)
{
    resize(750,700);
    QBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20,20,20,20);
    m_title = new QLabel(this);
    m_title->setText("<b>Make your analyses more insightful with ML. View your fluxomics workflow in PollyPhi</b>");
    m_title->setContentsMargins(0,0,0,10);
    m_title->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_title);

    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setContentsMargins(0,0,0,20);
    m_message->setText("Classify groups as Good and Bad quickly and with accuracy with the new Machine Learning \
algorithm. Save time classifying data and spend more time analyzing it. ");
    layout->addWidget(m_message);


    m_extraText =new QLabel(this);
    m_extraText->setText("To know more view the demo below");
    m_extraText->setAlignment(Qt::AlignCenter);

    layout->addWidget(m_extraText);

    m_vidWidget = new QVideoWidget(this);
    m_vidWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
    m_vidWidget->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_vidWidget);


    layout->itemAt(0)->setAlignment(Qt::AlignCenter);


    m_mediaPlayer = new QMediaPlayer(this, QMediaPlayer::VideoSurface);
    m_mediaPlayer->setVideoOutput(m_vidWidget);


    QBoxLayout* controlsLayout = new QHBoxLayout(this);

    m_playButton = new QPushButton;
    m_playButton->setEnabled(false);
    m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));

    controlsLayout->addWidget(m_playButton);

    m_positionSlider = new QSlider(Qt::Horizontal);
    m_positionSlider->setRange(0, 0);
    controlsLayout->addWidget(m_positionSlider);

    layout->addLayout(controlsLayout);

    QBoxLayout* footerLayout = new QHBoxLayout(this);

    m_checkBox = new QCheckBox(this);
    m_checkBox->setText("Don't show this again");
    m_checkBox->setChecked(m_settings->value("hideVideoPlayer", 0).toBool());
    footerLayout->addWidget(m_checkBox);

    m_closeButton = new QPushButton(this);
    m_closeButton->setText("Close");

    footerLayout->addWidget(m_closeButton);
    footerLayout->itemAt(1)->setAlignment(Qt::AlignRight);

    layout->addLayout(footerLayout);


    connect(m_playButton, &QAbstractButton::clicked,
            this, &VideoPlayer::play);
    connect(m_positionSlider, &QAbstractSlider::sliderMoved,
            this, static_cast<void (VideoPlayer::*)(int)>(&VideoPlayer::setPosition));
    connect(m_positionSlider, &QAbstractSlider::sliderPressed,
            this, static_cast<void (VideoPlayer::*)()>(&VideoPlayer::setPosition));
    connect(m_mediaPlayer, &QMediaPlayer::stateChanged,
            this, &VideoPlayer::mediaStateChanged);
    connect(m_mediaPlayer, &QMediaPlayer::positionChanged, this, &VideoPlayer::positionChanged);
    connect(m_mediaPlayer, &QMediaPlayer::durationChanged, this, &VideoPlayer::durationChanged);
    connect(m_mediaPlayer,QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error),
            this, &VideoPlayer::mediaError);

    connect(m_closeButton, &QPushButton::clicked, this , &VideoPlayer::close);

}

VideoPlayer::~VideoPlayer()
{
}


void VideoPlayer::closeEvent(QCloseEvent *event)
{
    std::cerr << "closing the video player";
    std::cerr << "checked state :" <<  m_checkBox->isChecked();
    m_settings->setValue("hideVideoPlayer", (int)m_checkBox->isChecked());
}

void VideoPlayer::setUrl(const QUrl &url)
{
    setWindowFilePath(url.isLocalFile() ? url.toLocalFile() : QString());
    m_mediaPlayer->setMedia(url);
    m_playButton->setEnabled(true);
}

void VideoPlayer::play()
{
    switch (m_mediaPlayer->state()) {
    case QMediaPlayer::PlayingState:
        m_mediaPlayer->pause();
        break;
    default:
        m_mediaPlayer->play();
        break;
    }
}

void VideoPlayer::mediaError(QMediaPlayer::Error err)
{
    qDebug() << "error : " << err << " " << m_mediaPlayer->errorString();
}

void VideoPlayer::mediaStateChanged(QMediaPlayer::State state)
{
    switch(state) {
    case QMediaPlayer::PlayingState:
        m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
        break;
    default:
        m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        break;
    }
}

void VideoPlayer::positionChanged(qint64 position)
{
    m_positionSlider->setValue(position);
}

void VideoPlayer::durationChanged(qint64 duration)
{
    m_positionSlider->setRange(0, duration);
}

void VideoPlayer::setPosition()
{
    m_mediaPlayer->setPosition(m_positionSlider->value());
}
void VideoPlayer::setPosition(int position)
{
    m_mediaPlayer->setPosition(position);
}
